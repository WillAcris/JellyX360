#ifndef JF_H
#define JF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JF_VERSION "0.2.0"
#define JF_CLIENT_NAME "JellyX360"
#define JF_DEVICE_NAME "Xbox 360"

typedef struct {
	char server[256]; // http://host:port, no trailing slash
	char user[64];
	char token[256];
	char user_id[64];
	char device_id[64];
} JfConfig;

typedef struct {
	char id[64];
	char name[192];
	char type[32];
	char overview[1024];
	long long runtime_ticks;
	long long resume_ticks;
	bool playable;
} JfItem;

typedef struct {
	JfItem *items;
	int count;
	int capacity;
} JfList;

// util.c
void jf_log(const char *format, ...);
size_t jf_strlcpy(char *dst, const char *src, size_t cap);
size_t jf_strlcat(char *dst, const char *src, size_t cap);
void jf_trim(char *text);
uint64_t jf_time_ms(void);
int jf_utf8_to_utf16(const char *src, uint16_t *dst, size_t cap);
int jf_utf16_to_utf8(const uint16_t *src, char *dst, size_t cap);
void jf_fit_text(char *dst, size_t cap, const char *src, int max_chars);

// config.c
void jf_config_defaults(JfConfig *config);
int jf_config_load(JfConfig *config);
int jf_config_save(const JfConfig *config);
void jf_make_device_id(char *out, size_t cap);

#endif
