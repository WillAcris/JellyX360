#ifndef JF_HTTP_H
#define JF_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	int status;
	char *body;
	size_t length;
	char content_type[128];
	char location[512];
	char error[256];
} HttpResponse;

// Plain HTTP/1.1 over xsdk's sockets. Jellyfin's default port 8096 needs no TLS; an https:// URL is refused with a
// message saying so.
int http_request(const char *method, const char *url, const char *headers, const char *body, size_t body_length,
	HttpResponse *response, int timeout_ms);
void http_response_free(HttpResponse *response);

typedef struct HttpStream HttpStream;

HttpStream *http_stream_open(const char *url, const char *headers, int timeout_ms);
// Bytes read, 0 at the end, -1 on a transport error.
int http_stream_read(HttpStream *stream, void *buffer, size_t length);
int http_stream_status(const HttpStream *stream);
void http_stream_close(HttpStream *stream);

#endif
