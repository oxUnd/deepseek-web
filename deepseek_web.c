#define _POSIX_C_SOURCE 200809L

#include "cJSON.h"

#include <curl/curl.h>
#include <ctype.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#define DEFAULT_API_BASE "https://api.deepseek.com"
#define MAX_CONFIG_BYTES (1024u * 1024u)
#define MAX_RESPONSE_BYTES (8u * 1024u * 1024u)

/*
 * Morph's native extension ABI is:
 *
 *     int ext_run(const char *args_json, char **result_json);
 *
 * It deliberately does not pass the runtime context.  The extension therefore
 * resolves the active config itself: MORPH_CONFIG / MORPH_CONFIG_PATH, then the
 * host process's -c/--config argument, then ~/.morph/config.toml.  The key is
 * taken from [model.text] first (api_key, then api_key_env), then
 * [model.vision], then DEEPSEEK_API_KEY as a last resort.
 */

struct strbuf {
	char *data;
	size_t len;
	size_t cap;
};

struct model_keys {
	char *api_key;
	char *api_key_env;
	char *adapter;
	char *provider;
};

struct http_buffer {
	struct strbuf body;
	int oom;
};

static int sb_reserve(struct strbuf *b, size_t need)
{
	size_t cap;

	if (need <= b->cap)
		return 1;
	cap = b->cap ? b->cap : 256;
	while (cap < need) {
		size_t next = cap * 2;
		if (next <= cap) {
			cap = need;
			break;
		}
		cap = next;
	}
	char *p = realloc(b->data, cap);
	if (!p)
		return 0;
	b->data = p;
	b->cap = cap;
	return 1;
}

static int sb_append(struct strbuf *b, const char *data, size_t len)
{
	if (len > SIZE_MAX - b->len - 1)
		return 0;
	if (!sb_reserve(b, b->len + len + 1))
		return 0;
	memcpy(b->data + b->len, data, len);
	b->len += len;
	b->data[b->len] = '\0';
	return 1;
}

static int sb_append_cstr(struct strbuf *b, const char *s)
{
	return sb_append(b, s, strlen(s));
}

static int sb_append_char(struct strbuf *b, char c)
{
	return sb_append(b, &c, 1);
}

static void sb_free(struct strbuf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static char *read_text_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	struct strbuf b = {0};
	char chunk[8192];
	size_t n;
	int ok = 1;

	if (!f)
		return NULL;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		if (n > MAX_CONFIG_BYTES - b.len || !sb_append(&b, chunk, n)) {
			ok = 0;
			break;
		}
	}
	if (ferror(f))
		ok = 0;
	fclose(f);
	if (!ok) {
		sb_free(&b);
		return NULL;
	}
	if (!b.data) {
		b.data = calloc(1, 1);
		if (!b.data)
			return NULL;
	}
	return b.data;
}

static char *trim(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return s;
}

static void strip_comment(char *s)
{
	int quote = 0;

	for (char *p = s; *p; p++) {
		if (quote) {
			if (quote == '"' && *p == '\\' && p[1]) {
				p++;
				continue;
			}
			if (*p == quote)
				quote = 0;
		} else if (*p == '"' || *p == '\'') {
			quote = *p;
		} else if (*p == '#') {
			*p = '\0';
			return;
		}
	}
}

static int append_utf8(struct strbuf *b, unsigned int cp)
{
	char out[4];
	size_t n;

	if (cp <= 0x7f) {
		out[0] = (char)cp;
		n = 1;
	} else if (cp <= 0x7ff) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		n = 2;
	} else if (cp <= 0xffff) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		n = 3;
	} else {
		out[0] = (char)(0xf0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[3] = (char)(0x80 | (cp & 0x3f));
		n = 4;
	}
	return sb_append(b, out, n);
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parse a TOML basic or literal string.  Returns a heap string or NULL. */
static char *parse_toml_string(const char *v)
{
	struct strbuf b = {0};

	while (*v && isspace((unsigned char)*v))
		v++;
	if (*v == '"') {
		v++;
		while (*v && *v != '"') {
			if (*v == '\\') {
				v++;
				if (!*v)
					break;
				switch (*v) {
				case 'n':
					sb_append_char(&b, '\n');
					break;
				case 't':
					sb_append_char(&b, '\t');
					break;
				case 'r':
					sb_append_char(&b, '\r');
					break;
				case 'b':
					sb_append_char(&b, '\b');
					break;
				case 'f':
					sb_append_char(&b, '\f');
					break;
				case '\\':
					sb_append_char(&b, '\\');
					break;
				case '"':
					sb_append_char(&b, '"');
					break;
				case '/':
					sb_append_char(&b, '/');
					break;
				case 'u': {
					unsigned int cp = 0;
					for (int i = 0; i < 4; i++) {
						int digit;
						v++;
						digit = hex_value(*v);
						if (digit < 0) {
							sb_free(&b);
							return NULL;
						}
						cp = cp * 16 + (unsigned int)digit;
					}
					append_utf8(&b, cp);
					break;
				}
				default:
					sb_append_char(&b, *v);
					break;
				}
				v++;
			} else {
				sb_append_char(&b, *v++);
			}
		}
		if (*v != '"') {
			sb_free(&b);
			return NULL;
		}
		if (!b.data) {
			b.data = calloc(1, 1);
			if (!b.data)
				return NULL;
		}
		return b.data;
	}
	if (*v == '\'') {
		const char *start;
		size_t len;
		char *out;

		v++;
		start = v;
		while (*v && *v != '\'')
			v++;
		if (*v != '\'')
			return NULL;
		len = (size_t)(v - start);
		out = malloc(len + 1);
		if (!out)
			return NULL;
		memcpy(out, start, len);
		out[len] = '\0';
		return out;
	}
	return NULL;
}

static void parse_config(const char *text, struct model_keys *text_keys,
			 struct model_keys *vision_keys)
{
	char *copy = strdup(text);
	char section[128] = {0};
	char *save = NULL;

	if (!copy)
		return;
	for (char *line = strtok_r(copy, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *s = trim(line);
		char *eq;
		char *key;
		char *value;
		struct model_keys *target;

		strip_comment(s);
		s = trim(s);
		if (*s == '[') {
			char *end = strchr(s, ']');
			if (end) {
				*end = '\0';
				snprintf(section, sizeof(section), "%s", trim(s + 1));
			}
			continue;
		}
		if (!*s)
			continue;
		if (strcmp(section, "model.text") == 0)
			target = text_keys;
		else if (strcmp(section, "model.vision") == 0)
			target = vision_keys;
		else
			continue;
		eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = '\0';
		key = trim(s);
		value = trim(eq + 1);
		if (strcmp(key, "api_key") == 0) {
			char *parsed = parse_toml_string(value);
			if (parsed) {
				free(target->api_key);
				target->api_key = parsed;
			}
		} else if (strcmp(key, "api_key_env") == 0) {
			char *parsed = parse_toml_string(value);
			if (parsed) {
				free(target->api_key_env);
				target->api_key_env = parsed;
			}
		} else if (strcmp(key, "adapter") == 0) {
			char *parsed = parse_toml_string(value);
			if (parsed) {
				free(target->adapter);
				target->adapter = parsed;
			}
		} else if (strcmp(key, "provider") == 0) {
			char *parsed = parse_toml_string(value);
			if (parsed) {
				free(target->provider);
				target->provider = parsed;
			}
		}
	}
	free(copy);
}

static char *join_path(const char *a, const char *b)
{
	size_t alen = strlen(a);
	size_t blen = strlen(b);
	int slash = alen > 0 && a[alen - 1] != '/' && blen > 0 && b[0] != '/';
	char *out = malloc(alen + blen + (size_t)slash + 1);
	size_t pos;

	if (!out)
		return NULL;
	memcpy(out, a, alen);
	pos = alen;
	if (slash)
		out[pos++] = '/';
	memcpy(out + pos, b, blen);
	out[pos + blen] = '\0';
	return out;
}

static char *expand_home(const char *path)
{
	const char *home;
	struct passwd *pw;

	if (path[0] != '~' || (path[1] != '/' && path[1] != '\0'))
		return strdup(path);
	home = getenv("HOME");
	if (!home || !*home) {
		pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : NULL;
	}
	if (!home || !*home)
		return NULL;
	return join_path(home, path[1] == '/' ? path + 1 : "");
}

#ifdef __APPLE__
static char *argv_config_path(void)
{
	int argc = *_NSGetArgc();
	char **argv = *_NSGetArgv();

	for (int i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) &&
		    i + 1 < argc)
			return strdup(argv[i + 1]);
		if (strncmp(argv[i], "--config=", 9) == 0)
			return strdup(argv[i] + 9);
	}
	return NULL;
}
#else
static char *argv_config_path(void)
{
	FILE *f = fopen("/proc/self/cmdline", "rb");
	char buf[8192];
	char *argv[256];
	int argc = 0;
	size_t n;
	char *p;

	if (!f)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	p = buf;
	while (p < buf + n && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
		argv[argc++] = p;
		p += strlen(p) + 1;
	}
	for (int i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) &&
		    i + 1 < argc)
			return strdup(argv[i + 1]);
		if (strncmp(argv[i], "--config=", 9) == 0)
			return strdup(argv[i] + 9);
	}
	return NULL;
}
#endif

static char *config_path(void)
{
	const char *env = getenv("MORPH_CONFIG");
	char *arg;
	char *out;
	const char *home;
	struct passwd *pw;

	if (env && *env)
		return expand_home(env);
	env = getenv("MORPH_CONFIG_PATH");
	if (env && *env)
		return expand_home(env);
	arg = argv_config_path();
	if (arg) {
		out = expand_home(arg);
		free(arg);
		return out;
	}
	home = getenv("HOME");
	if (!home || !*home) {
		pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : NULL;
	}
	if (!home || !*home)
		return NULL;
	return join_path(home, ".morph/config.toml");
}

static void free_model_keys(struct model_keys *keys)
{
	free(keys->api_key);
	free(keys->api_key_env);
	free(keys->adapter);
	free(keys->provider);
	keys->api_key = NULL;
	keys->api_key_env = NULL;
	keys->adapter = NULL;
	keys->provider = NULL;
}

/*
 * The tool is only meaningful when the active text model uses Morph's DeepSeek
 * adapter.  If adapter is omitted, Morph infers it from the provider: provider
 * "deepseek" means the DeepSeek adapter.  Any other explicit adapter is refused
 * before an HTTP request is made.
 */
static int deepseek_adapter_supported(const struct model_keys *text_keys,
				      char **error)
{
	const char *adapter = text_keys->adapter && text_keys->adapter[0]
				      ? text_keys->adapter
				      : NULL;
	const char *provider = text_keys->provider && text_keys->provider[0]
				       ? text_keys->provider
				       : NULL;
	int supported = adapter ? strcmp(adapter, "deepseek") == 0
				: provider ? strcmp(provider, "deepseek") == 0
					   : 0;

	if (supported)
		return 1;
	if (error) {
		struct strbuf msg = {0};
		sb_append_cstr(&msg,
			       "当前模型不支持 deepseek_web：需要 [model.text].adapter = "
			       "\"deepseek\"（当前 ");
		if (adapter) {
			sb_append_cstr(&msg, "adapter=");
			sb_append_cstr(&msg, adapter);
		} else {
			sb_append_cstr(&msg, "adapter=未配置");
		}
		if (provider) {
			sb_append_cstr(&msg, ", provider=");
			sb_append_cstr(&msg, provider);
		}
		sb_append_cstr(&msg, "）");
		*error = msg.data;
	}
	return 0;
}

static char *resolve_api_key(char **error)
{
	struct model_keys text_keys = {0};
	struct model_keys vision_keys = {0};
	struct model_keys *chosen;
	char *path = config_path();
	char *text = path ? read_text_file(path) : NULL;
	char *result = NULL;

	if (!text) {
		if (error)
			*error = strdup("无法读取当前 Morph 配置，无法确认 "
					"[model.text].adapter 是否为 deepseek");
		free(path);
		return NULL;
	}
	parse_config(text, &text_keys, &vision_keys);
	free(text);
	if (!deepseek_adapter_supported(&text_keys, error)) {
		free_model_keys(&text_keys);
		free_model_keys(&vision_keys);
		free(path);
		return NULL;
	}
	if (text_keys.api_key || text_keys.api_key_env)
		chosen = &text_keys;
	else
		chosen = &vision_keys;

	if (chosen->api_key && chosen->api_key[0]) {
		result = strdup(chosen->api_key);
	} else if (chosen->api_key_env && chosen->api_key_env[0]) {
		const char *value = getenv(chosen->api_key_env);
		if (value && *value) {
			result = strdup(value);
		} else if (error) {
			struct strbuf msg = {0};
			sb_append_cstr(&msg, "config uses api_key_env=");
			sb_append_cstr(&msg, chosen->api_key_env);
			sb_append_cstr(&msg, " but that environment variable is not set");
			*error = msg.data;
		}
	}
	if (!result && !(error && *error)) {
		const char *value = getenv("DEEPSEEK_API_KEY");
		if (value && *value) {
			result = strdup(value);
		} else if (error) {
			struct strbuf msg = {0};
			sb_append_cstr(&msg,
				       "no DeepSeek API key found: set [model.text].api_key or "
				       "[model.text].api_key_env");
			if (path) {
				sb_append_cstr(&msg, " in ");
				sb_append_cstr(&msg, path);
			}
			sb_append_cstr(&msg, ", or export DEEPSEEK_API_KEY");
			*error = msg.data;
		}
	}
	free_model_keys(&text_keys);
	free_model_keys(&vision_keys);
	free(path);
	return result;
}

static void set_error(char **error, const char *message)
{
	if (error && !*error)
		*error = strdup(message);
}

static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	struct http_buffer *response = userdata;
	size_t n;

	if (size && nmemb > SIZE_MAX / size)
		return 0;
	n = size * nmemb;
	if (n > MAX_RESPONSE_BYTES - response->body.len ||
	    !sb_append(&response->body, ptr, n)) {
		response->oom = 1;
		return 0;
	}
	return n;
}

static char *build_url(const char *endpoint)
{
	const char *base = getenv("DEEPSEEK_API_BASE");
	size_t blen;
	size_t elen;
	char *url;

	if (!base || !*base)
		base = DEFAULT_API_BASE;
	blen = strlen(base);
	while (blen > 0 && base[blen - 1] == '/')
		blen--;
	elen = strlen(endpoint);
	url = malloc(blen + elen + 1);
	if (!url)
		return NULL;
	memcpy(url, base, blen);
	memcpy(url + blen, endpoint, elen + 1);
	return url;
}

static char *post_json(const char *url, const char *api_key, const char *body,
		       char **error)
{
	CURL *curl;
	struct curl_slist *headers = NULL;
	struct http_buffer response = {0};
	char auth[2048];
	CURLcode rc;
	long status = 0;
	char *result = NULL;

	curl = curl_easy_init();
	if (!curl) {
		set_error(error, "curl initialization failed");
		return NULL;
	}
	if (strchr(api_key, '\r') || strchr(api_key, '\n')) {
		curl_easy_cleanup(curl);
		set_error(error, "API key contains a line break");
		return NULL;
	}
	if (snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key) >=
	    (int)sizeof(auth)) {
		curl_easy_cleanup(curl);
		set_error(error, "API key is too long");
		return NULL;
	}
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, auth);
	if (!headers) {
		curl_easy_cleanup(curl);
		set_error(error, "failed to build HTTP headers");
		return NULL;
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "morph-deepseek-web/1.0");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	rc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	memset(auth, 0, sizeof(auth));

	if (response.oom) {
		set_error(error, "DeepSeek response exceeds the 8 MiB limit");
	} else if (rc != CURLE_OK) {
		struct strbuf msg = {0};
		sb_append_cstr(&msg, "DeepSeek request failed: ");
		sb_append_cstr(&msg, curl_easy_strerror(rc));
		if (error && !*error)
			*error = msg.data;
		else
			sb_free(&msg);
	} else if (status != 200) {
		struct strbuf msg = {0};
		char status_text[64];
		size_t n;
		snprintf(status_text, sizeof(status_text), "DeepSeek API returned HTTP %ld",
			 status);
		sb_append_cstr(&msg, status_text);
		if (response.body.data && response.body.len) {
			sb_append_cstr(&msg, ": ");
			n = response.body.len > 2000 ? 2000 : response.body.len;
			sb_append(&msg, response.body.data, n);
		}
		if (error && !*error)
			*error = msg.data;
		else
			sb_free(&msg);
	} else if (!response.body.data || response.body.len == 0) {
		set_error(error, "DeepSeek returned an empty response");
	} else {
		result = response.body.data;
		response.body.data = NULL;
	}
	sb_free(&response.body);
	return result;
}

static char *error_json(const char *message)
{
	cJSON *root = cJSON_CreateObject();
	char *text;

	if (!root)
		return NULL;
	cJSON_AddStringToObject(root, "error", message ? message : "unknown error");
	text = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return text;
}

static const char *json_string(cJSON *root, const char *name)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

	return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static int json_int(cJSON *root, const char *name, int fallback)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

	return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static int json_bool(cJSON *root, const char *name, int fallback)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

	if (cJSON_IsBool(item))
		return cJSON_IsTrue(item) ? 1 : 0;
	return fallback;
}

int ext_run(const char *args_json, char **result_json)
{
	cJSON *args = NULL;
	cJSON *body = NULL;
	const char *action;
	const char *query;
	const char *url;
	char *body_text = NULL;
	char *request_url = NULL;
	char *api_key = NULL;
	char *key_error = NULL;
	char *http_error = NULL;
	char *response = NULL;
	int is_fetch;
	int rc = 0;

	if (!args_json || !result_json)
		return -1;
	*result_json = NULL;
	args = cJSON_Parse(args_json);
	if (!args || !cJSON_IsObject(args)) {
		*result_json = error_json("invalid JSON arguments");
		goto done;
	}
	action = json_string(args, "action");
	query = json_string(args, "query");
	url = json_string(args, "url");
	if (!action || !*action)
		action = (url && *url && (!query || !*query)) ? "fetch" : "search";
	if (strcmp(action, "search") == 0) {
		is_fetch = 0;
	} else if (strcmp(action, "fetch") == 0) {
		is_fetch = 1;
	} else {
		*result_json = error_json("action must be \"search\" or \"fetch\"");
		goto done;
	}
	if (is_fetch) {
		if (!url || !*url) {
			*result_json = error_json("fetch requires a non-empty url");
			goto done;
		}
	} else if (!query || !*query) {
		*result_json =
			error_json("search requires a non-empty query");
		goto done;
	}

	api_key = resolve_api_key(&key_error);
	if (!api_key) {
		*result_json = error_json(key_error ? key_error : "missing API key");
		goto done;
	}

	body = cJSON_CreateObject();
	if (!body) {
		*result_json = error_json("out of memory");
		rc = -1;
		goto done;
	}
	if (is_fetch) {
		cJSON_AddStringToObject(body, "url", url);
	} else {
		int count = json_int(args, "count", 10);
		const char *ori_question = json_string(args, "ori_question");
		const char *localization = json_string(args, "localization");

		if (count < 1)
			count = 1;
		if (count > 50)
			count = 50;
		cJSON_AddStringToObject(body, "query", query);
		cJSON_AddNumberToObject(body, "count", count);
		cJSON_AddBoolToObject(body, "need_content",
				      json_bool(args, "need_content", 0));
		if (ori_question && *ori_question)
			cJSON_AddStringToObject(body, "ori_question", ori_question);
		if (localization && *localization)
			cJSON_AddStringToObject(body, "localization", localization);
	}
	body_text = cJSON_PrintUnformatted(body);
	request_url =
		build_url(is_fetch ? "/v1/tools/fetch" : "/v1/tools/search");
	if (!body_text || !request_url) {
		*result_json = error_json("out of memory");
		rc = -1;
		goto done;
	}
	response = post_json(request_url, api_key, body_text, &http_error);
	if (!response) {
		*result_json =
			error_json(http_error ? http_error : "DeepSeek request failed");
	} else {
		*result_json = response;
		response = NULL;
	}

done:
	if (api_key) {
		memset(api_key, 0, strlen(api_key));
		free(api_key);
	}
	free(key_error);
	free(http_error);
	free(response);
	free(body_text);
	free(request_url);
	cJSON_Delete(body);
	cJSON_Delete(args);
	return rc;
}
