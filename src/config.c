#include "jf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONFIG_PATH "game:/jellyx360.cfg"
#define CONFIG_PATH_LEGACY "game:/jellyfin360.cfg"

void jf_config_defaults(JfConfig *config)
{
	memset(config, 0, sizeof *config);
	jf_make_device_id(config->device_id, sizeof config->device_id);
}

void jf_make_device_id(char *out, size_t cap)
{
	if (cap < 33) {
		if (cap)
			out[0] = '\0';
		return;
	}
	const uint64_t now = (uint64_t)time(NULL);
	const uint32_t a = (uint32_t)(now ^ (now >> 32) ^ jf_time_ms());
	const uint32_t b = (uint32_t)rand() ^ (uint32_t)(now >> 7);
	const uint32_t c = (uint32_t)rand() * 2654435761u;
	const uint32_t d = (uint32_t)(now * 1103515245u) ^ (uint32_t)jf_time_ms();
	snprintf(out, cap, "%08x%08x%08x%08x", (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d);
}

int jf_config_load(JfConfig *config)
{
	jf_config_defaults(config);
	FILE *file = fopen(CONFIG_PATH, "r");
	if (!file) {
		file = fopen(CONFIG_PATH_LEGACY, "r");
		if (file)
			jf_log("config: usando arquivo antigo %s", CONFIG_PATH_LEGACY);
	}
	if (!file)
		return -1;
	char line[768];
	while (fgets(line, sizeof line, file)) {
		jf_trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		char *equals = strchr(line, '=');
		if (!equals)
			continue;
		*equals = '\0';
		char *key = line;
		char *value = equals + 1;
		jf_trim(key);
		jf_trim(value);
		if (strcmp(key, "server") == 0)
			jf_strlcpy(config->server, value, sizeof config->server);
		else if (strcmp(key, "user") == 0)
			jf_strlcpy(config->user, value, sizeof config->user);
		else if (strcmp(key, "token") == 0)
			jf_strlcpy(config->token, value, sizeof config->token);
		else if (strcmp(key, "user_id") == 0)
			jf_strlcpy(config->user_id, value, sizeof config->user_id);
		else if (strcmp(key, "device_id") == 0)
			jf_strlcpy(config->device_id, value, sizeof config->device_id);
	}
	fclose(file);
	if (!config->device_id[0])
		jf_make_device_id(config->device_id, sizeof config->device_id);
	return 0;
}

int jf_config_save(const JfConfig *config)
{
	FILE *file = fopen(CONFIG_PATH, "w");
	if (!file) {
		jf_log("config: nao consegui escrever %s", CONFIG_PATH);
		return -1;
	}
	fprintf(file, "server=%s\nuser=%s\ntoken=%s\nuser_id=%s\ndevice_id=%s\n", config->server, config->user,
		config->token, config->user_id, config->device_id);
	fclose(file);
	return 0;
}
