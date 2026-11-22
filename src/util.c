#include "jf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "xsdk/log.h"

void jf_log(const char *format, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof buffer, format, args);
	va_end(args);
	xsdk_log_write("%s", buffer);
}

size_t jf_strlcpy(char *dst, const char *src, size_t cap)
{
	if (!cap)
		return 0;
	size_t length = strlen(src);
	const size_t copied = length < cap - 1 ? length : cap - 1;
	memcpy(dst, src, copied);
	dst[copied] = '\0';
	return length;
}

size_t jf_strlcat(char *dst, const char *src, size_t cap)
{
	const size_t used = strlen(dst);
	if (used >= cap)
		return cap + strlen(src);
	const size_t room = cap - used;
	const size_t copied = jf_strlcpy(dst + used, src, room);
	return used + copied;
}

void jf_trim(char *text)
{
	char *start = text;
	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
		start++;
	if (start != text)
		memmove(text, start, strlen(start) + 1);
	size_t length = strlen(text);
	while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r' ||
		text[length - 1] == '\n'))
		text[--length] = '\0';
}

uint64_t jf_time_ms(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static int utf8_bytes(uint8_t lead)
{
	if (lead < 0x80)
		return 1;
	if ((lead & 0xE0) == 0xC0)
		return 2;
	if ((lead & 0xF0) == 0xE0)
		return 3;
	if ((lead & 0xF8) == 0xF0)
		return 4;
	return 1;
}

int jf_utf8_to_utf16(const char *src, uint16_t *dst, size_t cap)
{
	size_t out = 0;
	const uint8_t *p = (const uint8_t *)src;
	while (*p) {
		uint32_t code;
		const int bytes = utf8_bytes(*p);
		if (bytes == 1) {
			code = *p++;
		} else if (bytes == 2 && (p[1] & 0xC0) == 0x80) {
			code = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
			p += 2;
		} else if (bytes == 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
			code = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
			p += 3;
		} else if (bytes == 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
			code = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
				((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
			p += 4;
		} else {
			code = 0xFFFD;
			p++;
		}
		if (code >= 0x10000) {
			if (out + 2 >= cap)
				break;
			code -= 0x10000;
			dst[out++] = (uint16_t)(0xD800 | (code >> 10));
			dst[out++] = (uint16_t)(0xDC00 | (code & 0x3FF));
		} else {
			if (out + 1 >= cap)
				break;
			dst[out++] = (uint16_t)code;
		}
	}
	if (cap)
		dst[out < cap ? out : cap - 1] = 0;
	return (int)out;
}

int jf_utf16_to_utf8(const uint16_t *src, char *dst, size_t cap)
{
	size_t out = 0;
	for (size_t i = 0; src[i]; i++) {
		uint32_t code = src[i];
		if (code >= 0xD800 && code <= 0xDBFF && src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
			code = 0x10000 + ((code - 0xD800) << 10) + (src[i + 1] - 0xDC00);
			i++;
		}
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
		if (out + (size_t)bytes >= cap)
			break;
		memcpy(dst + out, encoded, (size_t)bytes);
		out += (size_t)bytes;
	}
	if (cap)
		dst[out < cap ? out : cap - 1] = '\0';
	return (int)out;
}

void jf_fit_text(char *dst, size_t cap, const char *src, int max_chars)
{
	if (max_chars <= 0 || !cap) {
		if (cap)
			dst[0] = '\0';
		return;
	}
	int chars = 0;
	const char *p = src;
	while (*p && chars < max_chars) {
		const int bytes = utf8_bytes((uint8_t)*p);
		for (int i = 0; i < bytes && p[i]; i++)
			p++;
		chars++;
	}
	if (!*p) {
		jf_strlcpy(dst, src, cap);
		return;
	}
	size_t keep = (size_t)(p - src);
	while (keep && ((uint8_t)src[keep] & 0xC0) == 0x80)
		keep--;
	if (keep > cap - 1)
		keep = cap - 1;
	memcpy(dst, src, keep);
	dst[keep] = '\0';
	jf_strlcat(dst, "...", cap);
}
