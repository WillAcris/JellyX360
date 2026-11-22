#include "json.h"

#include <stdlib.h>
#include <string.h>

int json_parse(JsonDoc *doc, const char *text, size_t length)
{
	doc->tokens = NULL;
	doc->count = 0;
	jsmn_parser parser;
	jsmn_init(&parser);
	const int needed = jsmn_parse(&parser, text, length, NULL, 0);
	if (needed <= 0)
		return -1;
	doc->tokens = malloc((size_t)needed * sizeof *doc->tokens);
	if (!doc->tokens)
		return -1;
	jsmn_init(&parser);
	const int parsed = jsmn_parse(&parser, text, length, doc->tokens, (unsigned)needed);
	if (parsed < 0) {
		free(doc->tokens);
		doc->tokens = NULL;
		return -1;
	}
	doc->count = parsed;
	return 0;
}

void json_free(JsonDoc *doc)
{
	free(doc->tokens);
	doc->tokens = NULL;
	doc->count = 0;
}

static int token_end(const JsonDoc *doc, int token)
{
	const int start = token + 1;
	if (doc->tokens[token].type != JSMN_OBJECT && doc->tokens[token].type != JSMN_ARRAY)
		return start;
	int end = start;
	for (int i = 0; i < doc->tokens[token].size; i++) {
		end = token_end(doc, end);
		if (doc->tokens[token].type == JSMN_OBJECT)
			end = token_end(doc, end);
	}
	return end;
}

bool json_is_object(const JsonDoc *doc, int token)
{
	return token >= 0 && token < doc->count && doc->tokens[token].type == JSMN_OBJECT;
}

bool json_is_array(const JsonDoc *doc, int token)
{
	return token >= 0 && token < doc->count && doc->tokens[token].type == JSMN_ARRAY;
}

bool json_is_string(const JsonDoc *doc, int token)
{
	return token >= 0 && token < doc->count && doc->tokens[token].type == JSMN_STRING;
}

static bool token_equals(const char *text, const JsonDoc *doc, int token, const char *value)
{
	if (!json_is_string(doc, token))
		return false;
	const size_t length = strlen(value);
	return (size_t)(doc->tokens[token].end - doc->tokens[token].start) == length &&
		memcmp(text + doc->tokens[token].start, value, length) == 0;
}

int json_obj_get(const char *text, const JsonDoc *doc, int object, const char *key)
{
	if (!json_is_object(doc, object))
		return -1;
	int token = object + 1;
	for (int i = 0; i < doc->tokens[object].size; i++) {
		const int name = token;
		token = token_end(doc, token);
		if (token_equals(text, doc, name, key))
			return token;
		token = token_end(doc, token);
	}
	return -1;
}

int json_arr_size(const JsonDoc *doc, int array)
{
	return json_is_array(doc, array) ? doc->tokens[array].size : 0;
}

int json_arr_at(const JsonDoc *doc, int array, int index)
{
	if (!json_is_array(doc, array) || index < 0 || index >= doc->tokens[array].size)
		return -1;
	int token = array + 1;
	for (int i = 0; i < index; i++)
		token = token_end(doc, token);
	return token;
}

static void put_utf8(char *dst, size_t cap, size_t *out, unsigned code)
{
	char encoded[4];
	int bytes;
	if (code < 0x80) {
		encoded[0] = (char)code;
		bytes = 1;
	} else if (code < 0x800) {
		encoded[0] = (char)(0xC0 | (code >> 6));
		encoded[1] = (char)(0x80 | (code & 0x3F));
		bytes = 2;
	} else if (code < 0x10000) {
		encoded[0] = (char)(0xE0 | (code >> 12));
		encoded[1] = (char)(0x80 | ((code >> 6) & 0x3F));
		encoded[2] = (char)(0x80 | (code & 0x3F));
		bytes = 3;
	} else {
		encoded[0] = (char)(0xF0 | (code >> 18));
		encoded[1] = (char)(0x80 | ((code >> 12) & 0x3F));
		encoded[2] = (char)(0x80 | ((code >> 6) & 0x3F));
		encoded[3] = (char)(0x80 | (code & 0x3F));
		bytes = 4;
	}
	if (*out + (size_t)bytes < cap) {
		memcpy(dst + *out, encoded, (size_t)bytes);
		*out += (size_t)bytes;
	}
}

void json_string(const char *text, const JsonDoc *doc, int token, char *dst, size_t cap)
{
	if (!cap)
		return;
	dst[0] = '\0';
	if (!json_is_string(doc, token))
		return;
	size_t out = 0;
	for (int i = doc->tokens[token].start; i < doc->tokens[token].end && out + 1 < cap; i++) {
		const unsigned char c = (unsigned char)text[i];
		if (c != '\\') {
			dst[out++] = (char)c;
			continue;
		}
		if (++i >= doc->tokens[token].end)
			break;
		const char escape = text[i];
		if (escape == 'n') {
			dst[out++] = '\n';
		} else if (escape == 'r') {
			dst[out++] = '\r';
		} else if (escape == 't') {
			dst[out++] = '\t';
		} else if (escape == 'b') {
			dst[out++] = '\b';
		} else if (escape == 'f') {
			dst[out++] = '\f';
		} else if (escape == 'u' && i + 4 < doc->tokens[token].end) {
			unsigned code = 0;
			for (int digit = 0; digit < 4; digit++) {
				const char h = text[++i];
				code <<= 4;
				if (h >= '0' && h <= '9')
					code |= (unsigned)(h - '0');
				else if (h >= 'a' && h <= 'f')
					code |= (unsigned)(h - 'a' + 10);
				else if (h >= 'A' && h <= 'F')
					code |= (unsigned)(h - 'A' + 10);
			}
			if (code >= 0xD800 && code <= 0xDBFF && i + 6 < doc->tokens[token].end && text[i + 1] == '\\' &&
				text[i + 2] == 'u') {
				unsigned low = 0;
				for (int digit = 0; digit < 4; digit++) {
					const char h = text[i + 3 + digit];
					low <<= 4;
					if (h >= '0' && h <= '9')
						low |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f')
						low |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F')
						low |= (unsigned)(h - 'A' + 10);
				}
				if (low >= 0xDC00 && low <= 0xDFFF) {
					code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
					i += 6;
				}
			}
			put_utf8(dst, cap, &out, code);
		} else {
			dst[out++] = escape;
		}
	}
	dst[out] = '\0';
}

long long json_int(const char *text, const JsonDoc *doc, int token)
{
	if (token < 0 || token >= doc->count)
		return 0;
	char buffer[32];
	const int length = doc->tokens[token].end - doc->tokens[token].start;
	if (length <= 0 || length >= (int)sizeof buffer)
		return 0;
	memcpy(buffer, text + doc->tokens[token].start, (size_t)length);
	buffer[length] = '\0';
	return strtoll(buffer, NULL, 10);
}
