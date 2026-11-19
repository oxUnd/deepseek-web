/*
 * Test-only shim: exposes a context-free entrypoint so the Python integration
 * test can drive ext_run_v2 with a synthesized effective configuration.
 */

#define _POSIX_C_SOURCE 200809L

#include <morph/ext.h>

#include <nb/err.h>

#include <stdlib.h>
#include <string.h>

int ext_run_v2(const struct ext_context *context, const char *args_json,
	       char **result_json);

static int harness_check_cancelled(void *host_data)
{
	(void)host_data;
	return NB_OK;
}

static void harness_log(void *host_data, enum nb_log_level level,
			const char *message)
{
	(void)host_data;
	(void)level;
	(void)message;
}

static const char *env_value(const char *name)
{
	const char *value = getenv(name);

	return value ? value : "";
}

static const char *harness_get_config_string(void *host_data, const char *path)
{
	(void)host_data;
	static const struct {
		const char *path;
		const char *environment;
	} fields[] = {
		{"model.text.adapter", "HARNESS_TEXT_ADAPTER"},
		{"model.text.provider", "HARNESS_TEXT_PROVIDER"},
		{"model.text.api_key", "HARNESS_TEXT_API_KEY"},
		{"model.text.api_key_env", "HARNESS_TEXT_API_KEY_ENV"},
		{"model.vision.api_key", "HARNESS_VISION_API_KEY"},
		{"model.vision.api_key_env", "HARNESS_VISION_API_KEY_ENV"},
	};
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (strcmp(path, fields[i].path) == 0)
			return env_value(fields[i].environment);
	}
	return NULL;
}

int harness_run(const char *args_json, char **result_json)
{
	struct ext_host_api host = {0};
	struct ext_context context = {0};

	host.abi_version = EXT_CONTEXT_ABI_VERSION;
	host.struct_size = sizeof(host);
	host.check_cancelled = harness_check_cancelled;
	host.log = harness_log;
	host.get_config_string = harness_get_config_string;
	const char *mode = env_value("HARNESS_HOST_MODE");
	if (strcmp(mode, "old-host") == 0)
		host.struct_size = offsetof(struct ext_host_api, get_config_string);
	if (strcmp(mode, "missing-callback") == 0)
		host.get_config_string = NULL;

	context.abi_version = EXT_CONTEXT_ABI_VERSION;
	context.struct_size = sizeof(context);
	context.host = &host;

	return ext_run_v2(&context, args_json, result_json);
}
