#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "jf.h"

enum {
	HTTP_BUFFER_BYTES = 8192,
	HTTP_MAX_BODY_BYTES = 4 * 1024 * 1024,
	HTTP_MAX_REDIRECTS = 3,
};

struct HttpStream {
	int fd;
	uint8_t buffer[HTTP_BUFFER_BYTES];
	size_t position;
	size_t length;
	int status;
	int chunked;
	long long content_length;
	long long body_read;
	int chunk_left;
	int eof;
	int error;
	char error_text[256];
	char content_type[128];
	char location[512];
};

static int parse_url(const char *url, char *host, size_t host_cap, char *port, size_t port_cap, char *path,
	size_t path_cap, char *error, size_t error_cap)
{
	if (strncmp(url, "https://", 8) == 0) {
		snprintf(error, error_cap, "HTTPS nao e suportado nesta versao; use http:// no Jellyfin");
		return -1;
	}
	if (strncmp(url, "http://", 7) != 0) {
		snprintf(error, error_cap, "URL invalida (precisa comecar com http://)");
		return -1;
	}
	const char *authority = url + 7;
	const char *slash = strchr(authority, '/');
	const char *end = slash ? slash : authority + strlen(authority);
	const char *colon = NULL;
	for (const char *p = authority; p < end; p++)
		if (*p == ':')
			colon = p;
	const size_t host_length = (size_t)((colon ? colon : end) - authority);
	if (!host_length || host_length >= host_cap) {
		snprintf(error, error_cap, "Servidor invalido");
		return -1;
	}
	memcpy(host, authority, host_length);
	host[host_length] = '\0';
	if (colon) {
		const size_t port_length = (size_t)(end - colon - 1);
		if (!port_length || port_length >= port_cap) {
			snprintf(error, error_cap, "Porta invalida");
			return -1;
		}
		memcpy(port, colon + 1, port_length);
		port[port_length] = '\0';
	} else {
		jf_strlcpy(port, "80", port_cap);
	}
	if (slash)
		jf_strlcpy(path, slash, path_cap);
	else
		jf_strlcpy(path, "/", path_cap);
	return 0;
}

static int ci_equal(const char *a, const char *b)
{
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static int connect_to(const char *host, const char *port, int timeout_ms, char *error, size_t error_cap)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	struct addrinfo *addresses = NULL;
	const int resolved = getaddrinfo(host, port, &hints, &addresses);
	if (resolved != 0 || !addresses) {
		snprintf(error, error_cap, "Nao encontrei %s:%s", host, port);
		return -1;
	}
	int fd = -1;
	for (struct addrinfo *address = addresses; address; address = address->ai_next) {
		fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (fd < 0)
			continue;
		struct timeval timeout = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		int receive_buffer = 128 * 1024;
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer);
		if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(addresses);
	if (fd < 0)
		snprintf(error, error_cap, "Nao consegui conectar em %s:%s", host, port);
	return fd;
}

static int send_all(int fd, const void *data, size_t length)
{
	const uint8_t *bytes = data;
	while (length) {
		const ssize_t sent = send(fd, bytes, length, 0);
		if (sent <= 0)
			return -1;
		bytes += sent;
		length -= (size_t)sent;
	}
	return 0;
}

static int fill(HttpStream *stream)
{
	if (stream->position < stream->length)
		return 1;
	const ssize_t received = recv(stream->fd, stream->buffer, sizeof stream->buffer, 0);
	if (received > 0) {
		stream->position = 0;
		stream->length = (size_t)received;
		return 1;
	}
	if (received == 0) {
		stream->eof = 1;
		return 0;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		snprintf(stream->error_text, sizeof stream->error_text, "Tempo esgotado esperando o servidor");
	} else {
		snprintf(stream->error_text, sizeof stream->error_text, "Falha de rede (%d)", errno);
	}
	stream->error = 1;
	return -1;
}

static int read_raw(HttpStream *stream, void *buffer, size_t length)
{
	uint8_t *bytes = buffer;
	size_t done = 0;
	while (done < length) {
		if (stream->position >= stream->length) {
			const int filled = fill(stream);
			if (filled <= 0)
				break;
		}
		size_t chunk = stream->length - stream->position;
		if (chunk > length - done)
			chunk = length - done;
		memcpy(bytes + done, stream->buffer + stream->position, chunk);
		stream->position += chunk;
		done += chunk;
	}
	return (int)done;
}

static int read_line(HttpStream *stream, char *line, size_t cap)
{
	size_t out = 0;
	for (;;) {
		if (stream->position >= stream->length) {
			const int filled = fill(stream);
			if (filled <= 0)
				return -1;
		}
		const char c = (char)stream->buffer[stream->position++];
		if (c == '\n') {
			if (out && line[out - 1] == '\r')
				out--;
			line[out] = '\0';
			return (int)out;
		}
		if (out + 1 < cap)
			line[out++] = c;
	}
}

static void read_headers(HttpStream *stream)
{
	char line[512];
	if (read_line(stream, line, sizeof line) < 0)
		return;
	char *space = strchr(line, ' ');
	if (space)
		stream->status = (int)strtol(space + 1, NULL, 10);
	while (read_line(stream, line, sizeof line) > 0) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;
		*colon = '\0';
		char *value = colon + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		if (ci_equal(line, "Content-Length")) {
			stream->content_length = strtoll(value, NULL, 10);
		} else if (ci_equal(line, "Transfer-Encoding")) {
			if (strstr(value, "chunked") || strstr(value, "Chunked"))
				stream->chunked = 1;
		} else if (ci_equal(line, "Content-Type")) {
			jf_strlcpy(stream->content_type, value, sizeof stream->content_type);
		} else if (ci_equal(line, "Location")) {
			jf_strlcpy(stream->location, value, sizeof stream->location);
		}
	}
}

static int read_chunked(HttpStream *stream, void *buffer, size_t length)
{
	if (stream->chunk_left == 0) {
		char line[128];
		if (read_line(stream, line, sizeof line) < 0) {
			stream->eof = 1;
			return 0;
		}
		char *end = NULL;
		const unsigned long size = strtoul(line, &end, 16);
		if (size == 0) {
			while (read_line(stream, line, sizeof line) > 0 && line[0])
				;
			stream->eof = 1;
			return 0;
		}
		stream->chunk_left = (int)size;
	}
	size_t want = length < (size_t)stream->chunk_left ? length : (size_t)stream->chunk_left;
	const int got = read_raw(stream, buffer, want);
	if (got <= 0) {
		stream->eof = 1;
		return 0;
	}
	stream->chunk_left -= got;
	if (stream->chunk_left == 0) {
		char crlf[2];
		read_raw(stream, crlf, 2);
	}
	return got;
}

static int stream_read_body(HttpStream *stream, void *buffer, size_t length)
{
	if (stream->eof || stream->error)
		return 0;
	if (stream->chunked)
		return read_chunked(stream, buffer, length);
	size_t want = length;
	if (stream->content_length >= 0) {
		const long long left = stream->content_length - stream->body_read;
		if (left <= 0) {
			stream->eof = 1;
			return 0;
		}
		if ((long long)want > left)
			want = (size_t)left;
	}
	const int got = read_raw(stream, buffer, want);
	if (got <= 0) {
		stream->eof = 1;
		return 0;
	}
	stream->body_read += got;
	return got;
}

int http_stream_read(HttpStream *stream, void *buffer, size_t length)
{
	if (!stream || stream->error)
		return -1;
	if (!length)
		return 0;
	for (;;) {
		const int got = stream_read_body(stream, buffer, length);
		if (got > 0)
			return got;
		if (stream->eof || stream->error)
			return stream->error ? -1 : 0;
	}
}

static HttpStream *open_stream(const char *url, const char *headers, int timeout_ms, char *error, size_t error_cap)
{
	char host[256];
	char port[16];
	char path[1024];
	if (parse_url(url, host, sizeof host, port, sizeof port, path, sizeof path, error, error_cap) != 0)
		return NULL;
	HttpStream *stream = calloc(1, sizeof *stream);
	if (!stream) {
		snprintf(error, error_cap, "Sem memoria");
		return NULL;
	}
	stream->fd = -1;
	stream->content_length = -1;
	stream->fd = connect_to(host, port, timeout_ms, error, error_cap);
	if (stream->fd < 0) {
		free(stream);
		return NULL;
	}
	char request[2048];
	const int request_length = snprintf(request, sizeof request,
		"GET %s HTTP/1.1\r\nHost: %s:%s\r\nUser-Agent: JellyX360/%s\r\nAccept: */*\r\nConnection: close\r\n%s\r\n",
		path, host, port, JF_VERSION, headers ? headers : "");
	if (request_length <= 0 || (size_t)request_length >= sizeof request ||
		send_all(stream->fd, request, (size_t)request_length) != 0) {
		snprintf(error, error_cap, "Falha ao enviar a requisicao");
		close(stream->fd);
		free(stream);
		return NULL;
	}
	read_headers(stream);
	return stream;
}

HttpStream *http_stream_open(const char *url, const char *headers, int timeout_ms)
{
	char error[256];
	HttpStream *stream = open_stream(url, headers, timeout_ms, error, sizeof error);
	if (!stream)
		jf_log("http: %s", error);
	return stream;
}

int http_stream_status(const HttpStream *stream)
{
	return stream ? stream->status : 0;
}

void http_stream_close(HttpStream *stream)
{
	if (!stream)
		return;
	if (stream->fd >= 0)
		close(stream->fd);
	free(stream);
}

static int request_once(const char *method, const char *url, const char *headers, const char *body, size_t body_length,
	HttpResponse *response, int timeout_ms)
{
	char host[256];
	char port[16];
	char path[1024];
	if (parse_url(url, host, sizeof host, port, sizeof port, path, sizeof path, response->error,
			sizeof response->error) != 0)
		return -1;
	const int fd = connect_to(host, port, timeout_ms, response->error, sizeof response->error);
	if (fd < 0)
		return -1;

	const size_t header_cap = 2048 + (headers ? strlen(headers) : 0);
	char *request = malloc(header_cap);
	if (!request) {
		close(fd);
		snprintf(response->error, sizeof response->error, "Sem memoria");
		return -1;
	}
	int request_length = snprintf(request, header_cap,
		"%s %s HTTP/1.1\r\nHost: %s:%s\r\nUser-Agent: JellyX360/%s\r\nAccept: */*\r\nConnection: close\r\n%s",
		method, path, host, port, JF_VERSION, headers ? headers : "");
	// Bail out before the header_cap - request_length subtraction below can wrap around.
	if (request_length <= 0 || (size_t)request_length >= header_cap) {
		free(request);
		close(fd);
		snprintf(response->error, sizeof response->error, "Requisicao grande demais");
		return -1;
	}
	if (body_length) {
		request_length += snprintf(request + request_length, header_cap - (size_t)request_length,
			"Content-Length: %u\r\n\r\n", (unsigned)body_length);
	} else {
		request_length += snprintf(request + request_length, header_cap - (size_t)request_length, "\r\n");
	}
	if (request_length <= 0 || (size_t)request_length >= header_cap) {
		free(request);
		close(fd);
		snprintf(response->error, sizeof response->error, "Requisicao grande demais");
		return -1;
	}
	int failed = send_all(fd, request, (size_t)request_length) != 0;
	free(request);
	if (!failed && body_length)
		failed = send_all(fd, body, body_length) != 0;
	if (failed) {
		close(fd);
		snprintf(response->error, sizeof response->error, "Falha ao enviar a requisicao");
		return -1;
	}

	HttpStream stream;
	memset(&stream, 0, sizeof stream);
	stream.fd = fd;
	stream.content_length = -1;
	read_headers(&stream);
	response->status = stream.status;
	jf_strlcpy(response->content_type, stream.content_type, sizeof response->content_type);
	jf_strlcpy(response->location, stream.location, sizeof response->location);

	size_t capacity = 4096;
	response->body = malloc(capacity);
	response->length = 0;
	if (!response->body) {
		close(fd);
		snprintf(response->error, sizeof response->error, "Sem memoria");
		return -1;
	}
	for (;;) {
		if (response->length + 4096 + 1 > capacity) {
			if (capacity >= HTTP_MAX_BODY_BYTES)
				break;
			capacity *= 2;
			if (capacity > HTTP_MAX_BODY_BYTES)
				capacity = HTTP_MAX_BODY_BYTES;
			char *grown = realloc(response->body, capacity);
			if (!grown)
				break;
			response->body = grown;
		}
		const int got = stream_read_body(&stream, response->body + response->length, capacity - response->length - 1);
		if (got <= 0)
			break;
		response->length += (size_t)got;
	}
	response->body[response->length] = '\0';
	if (stream.error && !response->length) {
		jf_strlcpy(response->error, stream.error_text, sizeof response->error);
		close(fd);
		free(response->body);
		response->body = NULL;
		response->length = 0;
		return -1;
	}
	close(fd);
	return 0;
}

int http_request(const char *method, const char *url, const char *headers, const char *body, size_t body_length,
	HttpResponse *response, int timeout_ms)
{
	memset(response, 0, sizeof *response);
	char current[2048];
	jf_strlcpy(current, url, sizeof current);
	for (int redirect = 0; redirect <= HTTP_MAX_REDIRECTS; redirect++) {
		if (request_once(method, current, headers, body, body_length, response, timeout_ms) != 0)
			return -1;
		if (response->status < 300 || response->status >= 400 || !response->location[0])
			return 0;
		if (redirect == HTTP_MAX_REDIRECTS)
			break;
		char next[2048];
		if (strncmp(response->location, "http://", 7) == 0) {
			jf_strlcpy(next, response->location, sizeof next);
		} else if (response->location[0] == '/') {
			char host[256], port[16], path[1024];
			if (parse_url(current, host, sizeof host, port, sizeof port, path, sizeof path, response->error,
					sizeof response->error) != 0)
				return -1;
			snprintf(next, sizeof next, "http://%s:%s%s", host, port, response->location);
		} else {
			return 0;
		}
		http_response_free(response);
		jf_strlcpy(current, next, sizeof current);
	}
	return 0;
}

void http_response_free(HttpResponse *response)
{
	free(response->body);
	response->body = NULL;
	response->length = 0;
}
