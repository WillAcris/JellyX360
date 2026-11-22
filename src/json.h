#ifndef JF_JSON_H
#define JF_JSON_H

#include <stdbool.h>
#include <stddef.h>

// The parser is header-only: each translation unit that sees it gets its own static copy.
#define JSMN_STATIC
#include "jsmn.h"

typedef struct {
	jsmntok_t *tokens;
	int count;
} JsonDoc;

int json_parse(JsonDoc *doc, const char *text, size_t length);
void json_free(JsonDoc *doc);
bool json_is_object(const JsonDoc *doc, int token);
bool json_is_array(const JsonDoc *doc, int token);
bool json_is_string(const JsonDoc *doc, int token);
int json_obj_get(const char *text, const JsonDoc *doc, int object, const char *key);
int json_arr_size(const JsonDoc *doc, int array);
int json_arr_at(const JsonDoc *doc, int array, int index);
void json_string(const char *text, const JsonDoc *doc, int token, char *dst, size_t cap);
long long json_int(const char *text, const JsonDoc *doc, int token);

#endif
