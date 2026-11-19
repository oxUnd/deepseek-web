#define _POSIX_C_SOURCE 200809L

#include "cJSON.h"
#include <morph/ext.h>

#include <curl/curl.h>
#include <nb/buf.h>
#include <nb/err.h>
#include <nb/time.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_API_BASE "https://api.deepseek.com"
#define MAX_RESPONSE_BYTES (8u * 1024u * 1024u)
#define DEFAULT_TIMEOUT_MS 60000L
#define CONNECT_TIMEOUT_MS 15000L

/*
 * Morph's context-aware native extension ABI:
 *
 *     int ext_run_v2(const struct ext_context *context,
 *                    const char *args_json, char **result_json);
 *
 * The host exposes the effective configuration through get_config_string, so
 * the tool never depends on private config layouts or probes config files.
 * The API key
 * and the selected adapter come from [model.text], with [model.vision] as a
 * key-only fallback; DEEPSEEK_API_KEY remains the last resort.
 *
 * nbase supplies buffers and monotonic time, cJSON parses arguments, and
 * libcurl performs the HTTP request.
 */

static void set_error(char **error, const char *message)
{
	if (error && !*error) {
		const char *text = message ? message : "unknown error";
		*error = strdup(text);
	}
}

static char *error_json(const char *message)
{
	cJSON *root = cJSON_CreateObject();
	char *text;

	if (!root)
		return NULL;
	if (!cJSON_AddStringToObject(root, "error",
				     message ? message : "unknown error")) {
		cJSON_Delete(root);
		return NULL;
	}
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

/*
 * The tool is only meaningful when the active text model uses Morph's DeepSeek
 * adapter.  If adapter is omitted, Morph infers it from the provider: provider
 * "deepseek" means the DeepSeek adapter.  Any other explicit adapter is refused
 * before an HTTP request is made.
 */
static int deepseek_adapter_supported(const struct ext_context *context,
				      char **error)
{
	const char *adapter = context->host->get_config_string(
		context->host_data, "model.text.adapter");
	const char *provider = context->host->get_config_string(
		context->host_data, "model.text.provider");
	if (adapter && !*adapter)
		adapter = NULL;
	if (provider && !*provider)
		provider = NULL;
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

static char *key_from_entry(const char *api_key, const char *api_key_env)
{
	if (api_key && api_key[0])
		return strdup(api_key);
	if (api_key_env && api_key_env[0]) {
		const char *value = getenv(api_key_env);

		if (value && *value)
			return strdup(value);
	}
	return NULL;
}

static char *resolve_api_key(const struct ext_context *context, char **error)
{
	char *key = key_from_entry(
		context->host->get_config_string(context->host_data, "model.text.api_key"),
		context->host->get_config_string(context->host_data, "model.text.api_key_env"));

	if (!key)
		key = key_from_entry(
			context->host->get_config_string(context->host_data, "model.vision.api_key"),
			context->host->get_config_string(context->host_data, "model.vision.api_key_env"));
	if (!key) {
		const char *value = getenv("DEEPSEEK_API_KEY");

		if (value && *value)
			key = strdup(value);
	}
	if (!key)
		set_error(error,
			  "no DeepSeek API key found: set [model.text].api_key "
			  "or [model.text].api_key_env, or export "
			  "DEEPSEEK_API_KEY");
	return key;
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

struct http_call {
	const struct ext_context *context;
	int cancel_status;
};

static int http_progress_cb(void *userdata, curl_off_t dltotal,
			    curl_off_t dlnow, curl_off_t ultotal,
			    curl_off_t ulnow)
{
	struct http_call *call = userdata;
	int rc;

	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;
	rc = call->context->host->check_cancelled(call->context->host_data);
	if (rc != NB_OK) {
		call->cancel_status = rc;
		return 1;
	}
	return 0;
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

/*
 * Returns NB_OK for both success and ordinary request failures; the latter set
 * *error and leave *result NULL.  Cancellation and an expired host deadline are
 * returned as negative statuses so the runtime can report them as such.
 */
static int post_json(const char *url, const char *api_key, const char *body,
		     const struct ext_context *context, char **result,
		     char **error)
{
	CURL *curl;
	struct curl_slist *headers = NULL;
	struct curl_slist *tmp;
	nb_buf response;
	nb_buf auth;
	char *auth_header = NULL;
	struct http_call call = {.context = context, .cancel_status = NB_OK};
	CURLcode rc;
	long status = 0;
	long timeout_ms = DEFAULT_TIMEOUT_MS;
	long connect_ms = CONNECT_TIMEOUT_MS;
	int outcome = NB_OK;

	*result = NULL;
	nb_buf_init(&response);
	nb_buf_init(&auth);
	curl = curl_easy_init();
	if (!curl) {
		set_error(error, "curl initialization failed");
		post_json_cleanup(NULL, NULL, NULL, &auth, &response);
		return NB_OK;
	}
	if (context->call.deadline_ms > 0) {
		int64_t remaining =
			context->call.deadline_ms - nb_time_monotonic_ms();

		if (remaining <= 0) {
			set_error(error, "DeepSeek request deadline expired");
			post_json_cleanup(curl, headers, auth_header, &auth,
					  &response);
			return NB_ERR_TIMEOUT;
		}
		if (remaining < timeout_ms)
			timeout_ms = (long)remaining;
	}
	if (connect_ms > timeout_ms)
		connect_ms = timeout_ms;
	if (timeout_ms < 1)
		timeout_ms = 1;
	if (connect_ms < 1)
		connect_ms = 1;

	if (strchr(api_key, '\r') || strchr(api_key, '\n')) {
		set_error(error, "API key contains a line break");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NB_OK;
	}
	if (!nb_buf_append_cstr(&auth, "Authorization: ") ||
	    !nb_buf_append_cstr(&auth, "Bearer ") ||
	    !nb_buf_append_cstr(&auth, api_key) ||
	    nb_buf_len(&auth) > 4096) {
		set_error(error, "API key is too long");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NB_OK;
	}
	auth_header = strdup(nb_buf_cstr(&auth));
	if (!auth_header) {
		set_error(error, "out of memory");
		post_json_cleanup(curl, headers, NULL, &auth, &response);
		return NB_OK;
	}
	tmp = curl_slist_append(headers, "Content-Type: application/json");
	if (!tmp) {
		set_error(error, "failed to build HTTP headers");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NB_OK;
	}
	headers = tmp;
	tmp = curl_slist_append(headers, "Accept: application/json");
	if (!tmp) {
		set_error(error, "failed to build HTTP headers");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NB_OK;
	}
	headers = tmp;
	tmp = curl_slist_append(headers, auth_header);
	if (!tmp) {
		set_error(error, "failed to build HTTP headers");
		post_json_cleanup(curl, headers, auth_header, &auth, &response);
		return NB_OK;
	}
	headers = tmp;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_ms);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "morph-deepseek-web/2.0");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, http_progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &call);
	rc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

	if (call.cancel_status != NB_OK) {
		outcome = call.cancel_status;
	} else if (rc == CURLE_ABORTED_BY_CALLBACK) {
		int cancel_status =
			context->host->check_cancelled(context->host_data);

		outcome = cancel_status != NB_OK ? cancel_status
						 : NB_ERR_INTERRUPTED;
	} else if (!nb_buf_ok(&response)) {
		set_error(error,
			  "DeepSeek response exceeds the 8 MiB limit");
	} else if (rc != CURLE_OK) {
		if (rc == CURLE_OPERATION_TIMEDOUT &&
		    context->call.deadline_ms > 0 &&
		    nb_time_monotonic_ms() >= context->call.deadline_ms) {
			outcome = NB_ERR_TIMEOUT;
		} else {
			nb_buf msg;

			nb_buf_init(&msg);
			nb_buf_printf(&msg, "DeepSeek request failed: %s",
				      curl_easy_strerror(rc));
			set_error(error, nb_buf_cstr(&msg));
			nb_buf_destroy(&msg);
		}
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
		*result = strdup(nb_buf_cstr(&response));
		if (!*result)
			outcome = NB_ERR_NOMEM;
	}

	post_json_cleanup(curl, headers, auth_header, &auth, &response);
	return outcome;
}

struct ext_run_resources {
	cJSON *args;
	cJSON *body;
	char *body_text;
	char *request_url;
	char *api_key;
	char *key_error;
	char *http_error;
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
	free(resources->body_text);
	free(resources->request_url);
	cJSON_Delete(resources->body);
	cJSON_Delete(resources->args);
	memset(resources, 0, sizeof(*resources));
}

static int finish_error(struct ext_run_resources *resources,
			char **result_json, const char *message)
{
	*result_json = error_json(message ? message : "unknown error");
	ext_run_resources_cleanup(resources);
	return *result_json ? NB_OK : NB_ERR_NOMEM;
}

int ext_run_v2(const struct ext_context *context, const char *args_json,
	       char **result_json)
{
	struct ext_run_resources resources = {0};
	const char *action;
	const char *query;
	const char *url;
	int is_fetch = 0;
	int rc;

	if (!args_json || !result_json)
		return NB_ERR_INVAL;
	if (*result_json)
		return NB_ERR_EXISTS;
	if (!context || context->abi_version != EXT_CONTEXT_ABI_VERSION ||
	    context->struct_size < sizeof(*context) ||
	    !context->host ||
	    context->host->abi_version != EXT_CONTEXT_ABI_VERSION ||
	    context->host->struct_size < offsetof(struct ext_host_api, check_cancelled) +
					 sizeof(context->host->check_cancelled) ||
	    !context->host->check_cancelled)
		return NB_ERR_INVAL;

	if (!EXT_HOST_HAS_CALLBACK(context->host, get_config_string)) {
		*result_json = error_json("deepseek_web requires Morph ext SDK 1.1 "
			"get_config_string support; upgrade the Morph host");
		return *result_json ? NB_ERR_UNSUPPORTED : NB_ERR_NOMEM;
	}

	rc = context->host->check_cancelled(context->host_data);
	if (rc != NB_OK)
		return rc;

	resources.args = cJSON_Parse(args_json);
	if (!resources.args || !cJSON_IsObject(resources.args))
		return finish_error(&resources, result_json,
				    "invalid JSON arguments");
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
		return finish_error(&resources, result_json,
				    "action must be \"search\" or \"fetch\"");
	}
	if (is_fetch) {
		if (!url || !*url)
			return finish_error(&resources, result_json,
					    "fetch requires a non-empty url");
	} else if (!query || !*query) {
		return finish_error(&resources, result_json,
				    "search requires a non-empty query");
	}

	if (!deepseek_adapter_supported(context,
					&resources.key_error))
		return finish_error(&resources, result_json,
				    resources.key_error);

	resources.api_key =
		resolve_api_key(context, &resources.key_error);
	if (!resources.api_key)
		return finish_error(&resources, result_json,
				    resources.key_error
					    ? resources.key_error
					    : "missing API key");

	resources.body = cJSON_CreateObject();
	if (!resources.body) {
		ext_run_resources_cleanup(&resources);
		return NB_ERR_NOMEM;
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
		ext_run_resources_cleanup(&resources);
		return NB_ERR_NOMEM;
	}
	rc = post_json(resources.request_url, resources.api_key,
		       resources.body_text, context, result_json,
		       &resources.http_error);
	if (rc != NB_OK) {
		ext_run_resources_cleanup(&resources);
		return rc;
	}
	if (!*result_json) {
		*result_json =
			error_json(resources.http_error
					   ? resources.http_error
					   : "DeepSeek request failed");
		if (!*result_json) {
			ext_run_resources_cleanup(&resources);
			return NB_ERR_NOMEM;
		}
	}
	ext_run_resources_cleanup(&resources);
	return NB_OK;
}
