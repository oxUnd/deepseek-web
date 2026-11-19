/*
 * Test-only shim: exposes a context-free entrypoint so the Python integration
 * test can drive ext_run_v2 with a synthesized effective configuration.
 */

#define _POSIX_C_SOURCE 200809L

#include "config/config.h"
#include "ext/context.h"

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

static void copy_field(char *dest, size_t capacity, const char *value)
{
	size_t length;

	if (!value)
		value = "";
	length = strlen(value);
	if (length >= capacity)
		length = capacity - 1;
	memcpy(dest, value, length);
	dest[length] = '\0';
}

int harness_run(const char *args_json, char **result_json)
{
	static struct config config;
	static struct ext_host_api host;
	struct ext_context context = {0};

	memset(&config, 0, sizeof(config));
	memset(&host, 0, sizeof(host));

	copy_field(config.models.text.adapter,
		   sizeof(config.models.text.adapter),
		   env_value("HARNESS_TEXT_ADAPTER"));
	copy_field(config.models.text.provider,
		   sizeof(config.models.text.provider),
		   env_value("HARNESS_TEXT_PROVIDER"));
	copy_field(config.models.text.api_key,
		   sizeof(config.models.text.api_key),
		   env_value("HARNESS_TEXT_API_KEY"));
	copy_field(config.models.text.api_key_env,
		   sizeof(config.models.text.api_key_env),
		   env_value("HARNESS_TEXT_API_KEY_ENV"));
	copy_field(config.models.vision.api_key,
		   sizeof(config.models.vision.api_key),
		   env_value("HARNESS_VISION_API_KEY"));
	copy_field(config.models.vision.api_key_env,
		   sizeof(config.models.vision.api_key_env),
		   env_value("HARNESS_VISION_API_KEY_ENV"));

	host.abi_version = EXT_CONTEXT_ABI_VERSION;
	host.struct_size = sizeof(host);
	host.check_cancelled = harness_check_cancelled;
	host.log = harness_log;

	context.abi_version = EXT_CONTEXT_ABI_VERSION;
	context.struct_size = sizeof(context);
	context.config = &config;
	context.host = &host;

	return ext_run_v2(&context, args_json, result_json);
}
