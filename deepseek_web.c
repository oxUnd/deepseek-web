#define _POSIX_C_SOURCE 200809L

#include "cJSON.h"
#include <morph/ext.h>
#include <nb/err.h>
#include <nb/http.h>
#include <nb/str.h>
#include <nb/time.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_API_BASE "https://api.deepseek.com"
#define MAX_RESPONSE_BYTES (8u * 1024u * 1024u)
#define DEFAULT_TIMEOUT_MS 60000
#define CONNECT_TIMEOUT_MS 15000
#define POLL_INTERVAL_MS 50

/* Config strings are borrowed from this invocation's effective host snapshot.
 * nbase supplies HTTP and monotonic time; cJSON parses arguments. */
static void set_error(char **error, const char *message)
{
	if (error && !*error)
		*error = strdup(message ? message : "unknown error");
}

static void set_errorf(char **error, const char *format, ...)
{
	if (!error || *error)
		return;
	va_list args;
	va_list copy;
	va_start(args, format);
	va_copy(copy, args);
	int length = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (length >= 0) {
		*error = malloc((size_t)length + 1);
		if (*error)
			vsnprintf(*error, (size_t)length + 1, format, args);
	}
	va_end(args);
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
	set_errorf(error, "deepseek_web requires the DeepSeek adapter: set "
		   "[model.text].adapter = \"deepseek\" (current adapter=%s%s%s)",
		   adapter ? adapter : "not configured", provider ? ", provider=" : "",
		   provider ? provider : "");
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

struct http_resources {
	nb_http_client *client;
	nb_http_call *call;
	char *auth;
};

static void http_resources_cleanup(struct http_resources *resources)
{
	/* Freeing an unfinished call cancels it without dispatching callbacks. */
	nb_http_call_free(resources->call);
	nb_http_client_free(resources->client);
	if (resources->auth) {
		memset(resources->auth, 0, strlen(resources->auth));
		free(resources->auth);
	}
}

static int request_cancelled(const struct ext_context *context)
{
	int rc = context->host->check_cancelled(context->host_data);
	if (rc != NB_OK)
		return rc;
	if (context->call.deadline_ms > 0 &&
	    nb_time_monotonic_ms() >= context->call.deadline_ms)
		return NB_ERR_TIMEOUT;
	return NB_OK;
}

/* Ordinary request failures become tool error JSON; cancellation and the host
 * deadline retain their negative status. No host-owned allocation escapes. */
static int post_json(const char *url, const char *api_key, const char *body,
		     const struct ext_context *context, char **result,
		     char **error)
{
	struct http_resources resources
		__attribute__((cleanup(http_resources_cleanup))) = {0};
	nb_http_client_options options;
	nb_http_request request;
	nb_err err = {0};
	int timeout_ms = DEFAULT_TIMEOUT_MS;
	int rc = request_cancelled(context);

	*result = NULL;
	if (rc != NB_OK)
		return rc;
	if (context->call.deadline_ms > 0) {
		int64_t remaining =
			context->call.deadline_ms - nb_time_monotonic_ms();
		if (remaining <= 0)
			return NB_ERR_TIMEOUT;
		if (remaining < timeout_ms)
			timeout_ms = (int)remaining;
	}
	if (strchr(api_key, '\r') || strchr(api_key, '\n')) {
		set_error(error, "API key contains a line break");
		return NB_OK;
	}
	size_t key_length = strlen(api_key);
	if (key_length > 4096 - (sizeof("Authorization: Bearer ") - 1)) {
		set_error(error, "API key is too long");
		return NB_OK;
	}
	resources.auth = malloc(sizeof("Bearer ") + key_length);
	if (!resources.auth)
		return NB_ERR_NOMEM;
	memcpy(resources.auth, "Bearer ", sizeof("Bearer ") - 1);
	memcpy(resources.auth + sizeof("Bearer ") - 1, api_key, key_length + 1);
	nb_http_header headers[] = {
		{NB_STR_LIT("Content-Type"), NB_STR_LIT("application/json")},
		{NB_STR_LIT("Accept"), NB_STR_LIT("application/json")},
		{NB_STR_LIT("Authorization"), {resources.auth, strlen(resources.auth)}},
	};

	nb_http_client_options_init(&options);
	options.user_agent = "morph-deepseek-web/3.0";
	rc = nb_http_client_new(&options, &resources.client, &err);
	if (rc == NB_OK) {
		nb_http_request_init(&request, "POST", url);
		request.headers = (nb_http_headers){headers, sizeof(headers) / sizeof(headers[0])};
		request.body = (nb_str){body, strlen(body)};
		request.timeouts.connect_ms = timeout_ms < CONNECT_TIMEOUT_MS
						    ? timeout_ms : CONNECT_TIMEOUT_MS;
		request.timeouts.response_ms = 0;
		request.timeouts.idle_ms = 0;
		request.timeouts.total_ms = timeout_ms;
		request.redirect = NB_HTTP_REDIRECT_FOLLOW_SAME_ORIGIN;
		request.max_redirects = 3;
		request.max_body_bytes = MAX_RESPONSE_BYTES;
		rc = nb_http_client_send(resources.client, &request, NULL,
					 &resources.call, &err);
	}
	while (rc == NB_OK && !nb_http_call_result(resources.call)) {
		int cancelled = request_cancelled(context);
		if (cancelled != NB_OK)
			return cancelled;
		int poll_ms = POLL_INTERVAL_MS;
		if (context->call.deadline_ms > 0) {
			int64_t remaining =
				context->call.deadline_ms - nb_time_monotonic_ms();
			if (remaining <= 0)
				return NB_ERR_TIMEOUT;
			if (remaining < poll_ms)
				poll_ms = (int)remaining;
		}
		rc = nb_http_client_poll(resources.client, poll_ms, NULL, &err);
	}
	int cancelled = request_cancelled(context);
	if (cancelled != NB_OK)
		return cancelled;

	const nb_http_result *http_result = nb_http_call_result(resources.call);
	if (rc == NB_OK && http_result) {
		rc = http_result->status;
		err = http_result->error;
	}
	if (rc == NB_ERR_NOMEM)
		return rc;
	if (rc == NB_ERR_TOO_BIG) {
		set_error(error, "DeepSeek response exceeds the 8 MiB limit");
		return NB_OK;
	}
	if (rc != NB_OK) {
		set_errorf(error, "DeepSeek request failed: %s",
			   err.msg[0] ? err.msg : "HTTP transport error");
		return NB_OK;
	}
	if (!http_result || !http_result->response) {
		set_error(error, "DeepSeek returned no HTTP response");
		return NB_OK;
	}
	nb_str response = {0};
	rc = nb_http_call_body(resources.call, &response, &err);
	if (rc != NB_OK)
		return rc;
	if (http_result->response->status_code != 200) {
		int length = response.len > 2000 ? 2000 : (int)response.len;
		set_errorf(error, "DeepSeek API returned HTTP %d%s%.*s",
			   http_result->response->status_code, length ? ": " : "",
			   length, response.ptr ? response.ptr : "");
	} else if (response.len == 0) {
		set_error(error, "DeepSeek returned an empty response");
	} else {
		*result = malloc(response.len + 1);
		if (!*result)
			return NB_ERR_NOMEM;
		memcpy(*result, response.ptr, response.len);
		(*result)[response.len] = '\0';
	}
	return NB_OK;
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

NB_EXPORT int ext_run_v2(const struct ext_context *context, const char *args_json,
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
