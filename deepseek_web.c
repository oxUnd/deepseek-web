#define _POSIX_C_SOURCE 200809L

#include "cJSON.h"

#include <curl/curl.h>
#include <nb/buf.h>
#include <nb/file.h>
#include <nb/str.h>
#include <nb/toml.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <crt_externs.h>
#endif

#define DEFAULT_API_BASE "https://api.deepseek.com"
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
 *
 * Generic containers, strings, paths and TOML parsing come from nbase.  cJSON
 * remains because nbase has no JSON parser; libcurl remains because nbase has
 * no HTTP client.
 */

struct model_keys {
	char *api_key;
	char *api_key_env;
	char *adapter;
	char *provider;
};

static void set_error(char **error, const char *message)
{
	if (error && !*error) {
		const char *text = message ? message : "unknown error";
		*error = strdup(text);
	}
}

static char *dup_nb_str(nb_str value)
{
	char *out;

	if (!value.ptr)
		return NULL;
	out = malloc(value.len + 1);
	if (!out)
		return NULL;
	memcpy(out, value.ptr, value.len);
	out[value.len] = '\0';
	return out;
}

static char *toml_string_at(const nb_toml *doc, const char *section,
			    const char *key)
{
	nb_str keys[3];
	nb_str value = {0};
	const nb_toml_value *node;

	if (!doc)
		return NULL;
	keys[0] = nb_str_from_cstr("model");
	keys[1] = nb_str_from_cstr(section);
	keys[2] = nb_str_from_cstr(key);
	node = nb_toml_get_path(doc, keys, 3);
	if (!nb_toml_get_string(node, &value) || value.len == 0)
		return NULL;
	return dup_nb_str(value);
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

static int load_model_keys(const char *path, struct model_keys *text,
			   struct model_keys *vision, char **error)
{
	nb_err parse_error = {0};
	nb_toml *doc = nb_toml_load(path, &parse_error);

	if (!doc) {
		set_error(error, "无法读取当前 Morph 配置（或解析失败）");
		return 0;
	}
	text->api_key = toml_string_at(doc, "text", "api_key");
	text->api_key_env = toml_string_at(doc, "text", "api_key_env");
	text->adapter = toml_string_at(doc, "text", "adapter");
	text->provider = toml_string_at(doc, "text", "provider");
	vision->api_key = toml_string_at(doc, "vision", "api_key");
	vision->api_key_env = toml_string_at(doc, "vision", "api_key_env");
	nb_toml_free(doc);
	return 1;
}

/*
 * The tool is only meaningful when the active text model uses Morph's DeepSeek
 * adapter.  If adapter is omitted, Morph infers it from the provider: provider
 * "deepseek" means the DeepSeek adapter.  Any other explicit adapter is refused
 * before an HTTP request is made.
 */
static int deepseek_adapter_supported(const struct model_keys *text,
				      char **error)
{
	const char *adapter = text->adapter && text->adapter[0]
				      ? text->adapter
				      : NULL;
	const char *provider = text->provider && text->provider[0]
				       ? text->provider
				       : NULL;
	int supported = adapter ? strcmp(adapter, "deepseek") == 0
				: provider ? strcmp(provider, "deepseek") == 0
					   : 0;

	if (supported)
		return 1;
	if (error && !*error) {
		nb_buf msg;
		const char *text_value;

		nb_buf_init(&msg);
		nb_buf_append_cstr(&msg,
				   "当前模型不支持 deepseek_web：需要 "
				   "[model.text].adapter = \"deepseek\"（当前 ");
		if (adapter) {
			nb_buf_append_cstr(&msg, "adapter=");
			nb_buf_append_cstr(&msg, adapter);
		} else {
			nb_buf_append_cstr(&msg, "adapter=未配置");
		}
		if (provider) {
			nb_buf_append_cstr(&msg, ", provider=");
			nb_buf_append_cstr(&msg, provider);
		}
		nb_buf_append_cstr(&msg, "）");
		text_value = nb_buf_cstr(&msg);
		*error = strdup(text_value ? text_value
					   : "当前模型不支持 deepseek_web");
		nb_buf_destroy(&msg);
	}
	return 0;
}

#ifdef __APPLE__
static char *argv_config_path(void)
{
	int argc = *_NSGetArgc();
	char **argv = *_NSGetArgv();

	for (int i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-c") == 0 ||
		     strcmp(argv[i], "--config") == 0) &&
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
		if ((strcmp(argv[i], "-c") == 0 ||
		     strcmp(argv[i], "--config") == 0) &&
		    i + 1 < argc)
			return strdup(argv[i + 1]);
		if (strncmp(argv[i], "--config=", 9) == 0)
			return strdup(argv[i] + 9);
	}
	return NULL;
}
#endif

static char *expand_path(const char *path)
{
	nb_buf expanded;
	char *result = NULL;

	nb_buf_init(&expanded);
	if (nb_path_expand(&expanded, path, NULL) == NB_OK) {
		const char *text = nb_buf_cstr(&expanded);
		if (text)
			result = strdup(text);
	}
	nb_buf_destroy(&expanded);
	return result;
}

static char *config_path(void)
{
	const char *env = getenv("MORPH_CONFIG");
	char *arg;
	char *out;

	if (env && *env)
		return expand_path(env);
	env = getenv("MORPH_CONFIG_PATH");
	if (env && *env)
		return expand_path(env);
	arg = argv_config_path();
	if (arg) {
		out = expand_path(arg);
		free(arg);
		return out;
	}
	return expand_path("~/.morph/config.toml");
}

static char *resolve_api_key(char **error)
{
	struct model_keys text = {0};
	struct model_keys vision = {0};
	struct model_keys *chosen;
	char *path = config_path();
	char *result = NULL;

	if (!path) {
		set_error(error, "无法确定当前 Morph 配置文件路径");
		return NULL;
	}
	if (!load_model_keys(path, &text, &vision, error)) {
		free(path);
		return NULL;
	}
	if (!deepseek_adapter_supported(&text, error)) {
		free_model_keys(&text);
		free_model_keys(&vision);
		free(path);
		return NULL;
	}
	if (text.api_key || text.api_key_env)
		chosen = &text;
	else
		chosen = &vision;

	if (chosen->api_key && chosen->api_key[0]) {
		result = strdup(chosen->api_key);
	} else if (chosen->api_key_env && chosen->api_key_env[0]) {
		const char *value = getenv(chosen->api_key_env);

		if (value && *value) {
			result = strdup(value);
		} else {
			nb_buf msg;

			nb_buf_init(&msg);
			nb_buf_append_cstr(&msg, "config uses api_key_env=");
			nb_buf_append_cstr(&msg, chosen->api_key_env);
			nb_buf_append_cstr(
				&msg,
				" but that environment variable is not set");
			set_error(error, nb_buf_cstr(&msg));
			nb_buf_destroy(&msg);
		}
	}
	if (!result && !(error && *error)) {
		const char *value = getenv("DEEPSEEK_API_KEY");

		if (value && *value) {
			result = strdup(value);
		} else {
			nb_buf msg;

			nb_buf_init(&msg);
			nb_buf_append_cstr(
				&msg,
				"no DeepSeek API key found: set [model.text].api_key "
				"or [model.text].api_key_env");
			if (path) {
				nb_buf_append_cstr(&msg, " in ");
				nb_buf_append_cstr(&msg, path);
			}
			nb_buf_append_cstr(&msg,
					   ", or export DEEPSEEK_API_KEY");
			set_error(error, nb_buf_cstr(&msg));
			nb_buf_destroy(&msg);
		}
	}
	free_model_keys(&text);
	free_model_keys(&vision);
	free(path);
	return result;
}

static size_t http_write_cb(char *ptr, size_t size, size_t nmemb,
			    void *userdata)
{
	nb_buf *body = userdata;
	size_t n;

	if (size && nmemb > SIZE_MAX / size)
		return 0;
	n = size * nmemb;
	if (nb_buf_len(body) > MAX_RESPONSE_BYTES ||
	    n > MAX_RESPONSE_BYTES - nb_buf_len(body)) {
		nb_buf_set_error(body, NB_ERR_TOO_BIG);
		return 0;
	}
	if (!nb_buf_append(body, ptr, n))
		return 0;
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

static void post_json_cleanup(CURL *curl, struct curl_slist *headers,
			      char *auth_header, nb_buf *auth, nb_buf *response)
{
	curl_slist_free_all(headers);
	if (curl)
		curl_easy_cleanup(curl);
	if (auth_header) {
		memset(auth_header, 0, strlen(auth_header));
		free(auth_header);
	}
	nb_buf_destroy(auth);
	nb_buf_destroy(response);
}

static char *post_json(const char *url, const char *api_key, const char *body,
		       char **error)
{
	CURL *curl;
	struct curl_slist *headers = NULL;
	nb_buf response;
	nb_buf auth;
	char *auth_header = NULL;
	CURLcode rc;
	long status = 0;
	char *result = NULL;

	nb_buf_init(&response);
	nb_buf_init(&auth);
	curl = curl_easy_init();
	if (!curl) {
		set_error(error, "curl initialization failed");
		post_json_cleanup(NULL, NULL, NULL, &auth, &response);
		return NULL;
	}
	if (strchr(api_key, '\r') || strchr(api_key, '\n')) {
		set_error(error, "API key contains a line break");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NULL;
	}
	if (!nb_buf_append_cstr(&auth, "Authorization: ") ||
	    !nb_buf_append_cstr(&auth, "Bearer ") ||
	    !nb_buf_append_cstr(&auth, api_key) ||
	    nb_buf_len(&auth) > 4096) {
		set_error(error, "API key is too long");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NULL;
	}
	auth_header = strdup(nb_buf_cstr(&auth));
	if (!auth_header) {
		set_error(error, "out of memory");
		post_json_cleanup(curl, headers, NULL, &auth, &response);
		return NULL;
	}
	struct curl_slist *tmp =
		curl_slist_append(headers, "Content-Type: application/json");
	if (!tmp) {
		set_error(error, "failed to build HTTP headers");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NULL;
	}
	headers = tmp;
	tmp = curl_slist_append(headers, "Accept: application/json");
	if (!tmp) {
		set_error(error, "failed to build HTTP headers");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NULL;
	}
	headers = tmp;
	tmp = curl_slist_append(headers, auth_header);
	if (!tmp) {
		set_error(error, "failed to build HTTP headers");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NULL;
	}
	headers = tmp;
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

	if (!nb_buf_ok(&response)) {
		set_error(error,
			  "DeepSeek response exceeds the 8 MiB limit");
	} else if (rc != CURLE_OK) {
		nb_buf msg;

		nb_buf_init(&msg);
		nb_buf_printf(&msg, "DeepSeek request failed: %s",
			      curl_easy_strerror(rc));
		set_error(error, nb_buf_cstr(&msg));
		nb_buf_destroy(&msg);
	} else if (status != 200) {
		nb_buf msg;

		nb_buf_init(&msg);
		nb_buf_append_cstr(&msg, "DeepSeek API returned HTTP ");
		nb_buf_append_i64(&msg, (int64_t)status);
		if (nb_buf_len(&response) > 0) {
			size_t n = nb_buf_len(&response) > 2000
					   ? 2000
					   : nb_buf_len(&response);
			nb_buf_append_cstr(&msg, ": ");
			nb_buf_append(&msg, nb_buf_cstr(&response), n);
		}
		set_error(error, nb_buf_cstr(&msg));
		nb_buf_destroy(&msg);
	} else if (nb_buf_len(&response) == 0) {
		set_error(error, "DeepSeek returned an empty response");
	} else {
		result = strdup(nb_buf_cstr(&response));
	}

	if (!result)
		set_error(error, "out of memory");
	post_json_cleanup(curl, headers, auth_header, &auth, &response);
	return result;
}

static char *error_json(const char *message)
{
	cJSON *root = cJSON_CreateObject();
	char *text;

	if (!root)
		return strdup("{\"error\":\"out of memory\"}");
	cJSON_AddStringToObject(root, "error",
				message ? message : "unknown error");
	text = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return text;
}

static const char *json_string(cJSON *root, const char *name)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

	return cJSON_IsString(item) && item->valuestring ? item->valuestring
							 : NULL;
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

struct ext_run_resources {
	cJSON *args;
	cJSON *body;
	char *body_text;
	char *request_url;
	char *api_key;
	char *key_error;
	char *http_error;
	char *response;
};

static void ext_run_resources_cleanup(struct ext_run_resources *resources)
{
	if (!resources)
		return;
	if (resources->api_key) {
		memset(resources->api_key, 0, strlen(resources->api_key));
		free(resources->api_key);
	}
	free(resources->key_error);
	free(resources->http_error);
	free(resources->response);
	free(resources->body_text);
	free(resources->request_url);
	cJSON_Delete(resources->body);
	cJSON_Delete(resources->args);
	memset(resources, 0, sizeof(*resources));
}

int ext_run(const char *args_json, char **result_json)
{
	struct ext_run_resources resources = {0};
	const char *action;
	const char *query;
	const char *url;
	int is_fetch = 0;

	if (!args_json || !result_json)
		return -1;
	*result_json = NULL;
	resources.args = cJSON_Parse(args_json);
	if (!resources.args || !cJSON_IsObject(resources.args)) {
		*result_json = error_json("invalid JSON arguments");
		ext_run_resources_cleanup(&resources);
		return 0;
	}
	action = json_string(resources.args, "action");
	query = json_string(resources.args, "query");
	url = json_string(resources.args, "url");
	if (!action || !*action)
		action = (url && *url && (!query || !*query)) ? "fetch"
							     : "search";
	if (strcmp(action, "search") == 0) {
		is_fetch = 0;
	} else if (strcmp(action, "fetch") == 0) {
		is_fetch = 1;
	} else {
		*result_json =
			error_json("action must be \"search\" or \"fetch\"");
		ext_run_resources_cleanup(&resources);
		return 0;
	}
	if (is_fetch) {
		if (!url || !*url) {
			*result_json =
				error_json("fetch requires a non-empty url");
			ext_run_resources_cleanup(&resources);
			return 0;
		}
	} else if (!query || !*query) {
		*result_json =
			error_json("search requires a non-empty query");
		ext_run_resources_cleanup(&resources);
		return 0;
	}

	resources.api_key = resolve_api_key(&resources.key_error);
	if (!resources.api_key) {
		*result_json =
			error_json(resources.key_error ? resources.key_error
						       : "missing API key");
		ext_run_resources_cleanup(&resources);
		return 0;
	}

	resources.body = cJSON_CreateObject();
	if (!resources.body) {
		*result_json = error_json("out of memory");
		ext_run_resources_cleanup(&resources);
		return -1;
	}
	if (is_fetch) {
		cJSON_AddStringToObject(resources.body, "url", url);
	} else {
		int count = json_int(resources.args, "count", 10);
		const char *ori_question =
			json_string(resources.args, "ori_question");
		const char *localization =
			json_string(resources.args, "localization");

		if (count < 1)
			count = 1;
		if (count > 50)
			count = 50;
		cJSON_AddStringToObject(resources.body, "query", query);
		cJSON_AddNumberToObject(resources.body, "count", count);
		cJSON_AddBoolToObject(
			resources.body, "need_content",
			json_bool(resources.args, "need_content", 0));
		if (ori_question && *ori_question)
			cJSON_AddStringToObject(resources.body, "ori_question",
						ori_question);
		if (localization && *localization)
			cJSON_AddStringToObject(resources.body, "localization",
						localization);
	}
	resources.body_text = cJSON_PrintUnformatted(resources.body);
	resources.request_url =
		build_url(is_fetch ? "/v1/tools/fetch" : "/v1/tools/search");
	if (!resources.body_text || !resources.request_url) {
		*result_json = error_json("out of memory");
		ext_run_resources_cleanup(&resources);
		return -1;
	}
	resources.response = post_json(
		resources.request_url, resources.api_key, resources.body_text,
		&resources.http_error);
	if (!resources.response) {
		*result_json =
			error_json(resources.http_error ? resources.http_error
							: "DeepSeek request failed");
	} else {
		*result_json = resources.response;
		resources.response = NULL;
	}
	ext_run_resources_cleanup(&resources);
	return 0;
}
