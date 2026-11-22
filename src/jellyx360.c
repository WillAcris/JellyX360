#include "jellyx360.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "json.h"

static void json_escape(const char *src, char *dst, size_t cap)
{
	size_t out = 0;
	for (; *src && out + 2 < cap; src++) {
		const unsigned char c = (unsigned char)*src;
		if (c == '"' || c == '\\') {
			dst[out++] = '\\';
			dst[out++] = (char)c;
		} else if (c == '\n') {
			dst[out++] = '\\';
			dst[out++] = 'n';
		} else if (c == '\r') {
			dst[out++] = '\\';
			dst[out++] = 'r';
		} else if (c == '\t') {
			dst[out++] = '\\';
			dst[out++] = 't';
		} else if (c >= 0x20) {
			dst[out++] = (char)c;
		}
	}
	dst[out] = '\0';
}

static void trim_server(JfConfig *config, const char *server)
{
	jf_strlcpy(config->server, server, sizeof config->server);
	jf_trim(config->server);
	size_t length = strlen(config->server);
	while (length && config->server[length - 1] == '/')
		config->server[--length] = '\0';
}

int jf_login(JfConfig *config, const char *server, const char *user, const char *password, char *error,
	size_t error_cap)
{
	trim_server(config, server);
	jf_strlcpy(config->user, user, sizeof config->user);
	jf_trim(config->user);
	if (!config->server[0] || !config->user[0]) {
		jf_strlcpy(error, "Preencha servidor e usuario", error_cap);
		return -1;
	}

	char escaped_user[256];
	char escaped_password[256];
	json_escape(config->user, escaped_user, sizeof escaped_user);
	json_escape(password, escaped_password, sizeof escaped_password);
	char body[768];
	snprintf(body, sizeof body, "{\"Username\":\"%s\",\"Pw\":\"%s\"}", escaped_user, escaped_password);

	char url[512];
	snprintf(url, sizeof url, "%s/Users/AuthenticateByName", config->server);
	char authorization[512];
	snprintf(authorization, sizeof authorization,
		"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"\r\n"
		"Content-Type: application/json\r\n",
		JF_CLIENT_NAME, JF_DEVICE_NAME, config->device_id, JF_VERSION);

	HttpResponse response;
	if (http_request("POST", url, authorization, body, strlen(body), &response, 3000) != 0) {
		snprintf(error, error_cap, "%s", response.error[0] ? response.error : "Falha de rede");
		http_response_free(&response);
		return -1;
	}
	if (response.status != 200) {
		snprintf(error, error_cap, "Login recusado (HTTP %d)", response.status);
		http_response_free(&response);
		return -1;
	}

	JsonDoc doc;
	if (json_parse(&doc, response.body, response.length) != 0) {
		snprintf(error, error_cap, "Resposta de login invalida");
		http_response_free(&response);
		return -1;
	}
	const int token = json_obj_get(response.body, &doc, 0, "AccessToken");
	const int user_object = json_obj_get(response.body, &doc, 0, "User");
	const int user_id = user_object >= 0 ? json_obj_get(response.body, &doc, user_object, "Id") : -1;
	if (token < 0 || user_id < 0) {
		snprintf(error, error_cap, "Resposta de login sem token");
		json_free(&doc);
		http_response_free(&response);
		return -1;
	}
	json_string(response.body, &doc, token, config->token, sizeof config->token);
	json_string(response.body, &doc, user_id, config->user_id, sizeof config->user_id);
	json_free(&doc);
	http_response_free(&response);
	if (!config->token[0] || !config->user_id[0]) {
		snprintf(error, error_cap, "Login sem token");
		return -1;
	}
	jf_log("jellyx360: entrou como %s em %s", config->user, config->server);
	return 0;
}

void jf_auth_header(const JfConfig *config, char *out, size_t cap)
{
	snprintf(out, cap,
		"Authorization: MediaBrowser Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\", Token=\"%s\"\r\n",
		JF_CLIENT_NAME, JF_DEVICE_NAME, config->device_id, JF_VERSION, config->token);
}

static void copy_item(const char *text, const JsonDoc *doc, int object, JfItem *item)
{
	memset(item, 0, sizeof *item);
	json_string(text, doc, json_obj_get(text, doc, object, "Id"), item->id, sizeof item->id);
	json_string(text, doc, json_obj_get(text, doc, object, "Name"), item->name, sizeof item->name);
	json_string(text, doc, json_obj_get(text, doc, object, "Type"), item->type, sizeof item->type);
	json_string(text, doc, json_obj_get(text, doc, object, "Overview"), item->overview, sizeof item->overview);
	item->runtime_ticks = json_int(text, doc, json_obj_get(text, doc, object, "RunTimeTicks"));
	const int user_data = json_obj_get(text, doc, object, "UserData");
	if (user_data >= 0)
		item->resume_ticks = json_int(text, doc, json_obj_get(text, doc, user_data, "PlaybackPositionTicks"));
	item->playable = strcmp(item->type, "Movie") == 0 || strcmp(item->type, "Episode") == 0 ||
		strcmp(item->type, "Video") == 0 || strcmp(item->type, "MusicVideo") == 0 ||
		strcmp(item->type, "TvChannel") == 0 || strcmp(item->type, "Program") == 0;
}

static int parse_items(const char *text, const JsonDoc *doc, int root, JfList *list)
{
	jf_list_free(list);
	const int items = json_obj_get(text, doc, root, "Items");
	const int count = json_arr_size(doc, items);
	if (count <= 0)
		return 0;
	list->items = calloc((size_t)count, sizeof *list->items);
	if (!list->items)
		return -1;
	list->capacity = count;
	for (int i = 0; i < count; i++) {
		const int object = json_arr_at(doc, items, i);
		if (object < 0)
			continue;
		copy_item(text, doc, object, &list->items[list->count]);
		if (list->items[list->count].id[0])
			list->count++;
	}
	return list->count;
}

static int do_list(const JfConfig *config, const char *url, JfList *list, char *error, size_t error_cap, const char *label)
{
	char authorization[512];
	jf_auth_header(config, authorization, sizeof authorization);
	HttpResponse response;
	if (http_request("GET", url, authorization, NULL, 0, &response, 5000) != 0) {
		snprintf(error, error_cap, "%s", response.error[0] ? response.error : "Falha de rede");
		http_response_free(&response);
		return -1;
	}
	if (response.status != 200) {
		snprintf(error, error_cap, "%s: HTTP %d", label, response.status);
		http_response_free(&response);
		return -1;
	}
	JsonDoc doc;
	if (json_parse(&doc, response.body, response.length) != 0) {
		snprintf(error, error_cap, "%s: resposta invalida", label);
		http_response_free(&response);
		return -1;
	}
	const int count = parse_items(response.body, &doc, 0, list);
	json_free(&doc);
	http_response_free(&response);
	if (count < 0) {
		snprintf(error, error_cap, "Sem memoria");
		return -1;
	}
	return count;
}

int jf_get_views(const JfConfig *config, JfList *list, char *error, size_t error_cap)
{
	char url[768];
	snprintf(url, sizeof url, "%s/Users/%s/Views", config->server, config->user_id);
	return do_list(config, url, list, error, error_cap, "Bibliotecas");
}

int jf_get_children(const JfConfig *config, const char *parent_id, const char *include_types, JfList *list, char *error, size_t error_cap)
{
	char url[2048];
	const char *types = (include_types && include_types[0]) ? include_types : "Movie,Episode,Series,Season,Folder,CollectionFolder,BoxSet,Video,MusicVideo";
	snprintf(url, sizeof url,
		"%s/Users/%s/Items?ParentId=%s&Recursive=false&IncludeItemTypes=%s&Fields=RunTimeTicks,Overview,UserData&"
		"SortBy=SortName&SortOrder=Ascending&Limit=300",
		config->server, config->user_id, parent_id, types);
	return do_list(config, url, list, error, error_cap, "Itens");
}

int jf_get_resume(const JfConfig *config, JfList *list, char *error, size_t error_cap)
{
	char url[2048];
	snprintf(url, sizeof url,
		"%s/Users/%s/Items/Resume?Limit=24&Fields=RunTimeTicks,Overview,UserData",
		config->server, config->user_id);
	return do_list(config, url, list, error, error_cap, "Resume");
}

int jf_get_item(const JfConfig *config, const char *item_id, JfItem *item, char *error, size_t error_cap)
{
	char url[1024];
	char authorization[512];
	jf_auth_header(config, authorization, sizeof authorization);
	snprintf(url, sizeof url, "%s/Items/%s?Fields=Overview,RunTimeTicks,UserData", config->server, item_id);
	HttpResponse response;
	if (http_request("GET", url, authorization, NULL, 0, &response, 5000) != 0) {
		snprintf(error, error_cap, "%s", response.error[0] ? response.error : "Falha de rede");
		http_response_free(&response);
		return -1;
	}
	if (response.status != 200) {
		snprintf(error, error_cap, "Item: HTTP %d", response.status);
		http_response_free(&response);
		return -1;
	}
	JsonDoc doc;
	if (json_parse(&doc, response.body, response.length) != 0) {
		snprintf(error, error_cap, "Resposta invalida");
		http_response_free(&response);
		return -1;
	}
	memset(item, 0, sizeof *item);
	copy_item(response.body, &doc, 0, item);
	json_free(&doc);
	http_response_free(&response);
	return 0;
}

int jf_get_live_channels(const JfConfig *config, JfList *list, char *error, size_t error_cap)
{
	char url[2048];
	snprintf(url, sizeof url,
		"%s/LiveTv/Channels?UserId=%s&EnableImages=true&ImageTypeLimit=1&EnableImageTypes=Primary",
		config->server, config->user_id);
	return do_list(config, url, list, error, error_cap, "Canais");
}

int jf_get_episodes(const JfConfig *config, const char *series_id, JfList *list, char *error, size_t error_cap)
{
	char url[2048];
	char authorization[512];
	jf_auth_header(config, authorization, sizeof authorization);
	snprintf(url, sizeof url,
		"%s/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=Episode&Fields=RunTimeTicks,Overview,UserData&SortBy=ParentIndexNumber,SortBy=IndexNumber",
		config->server, config->user_id, series_id);
	HttpResponse response;
	if (http_request("GET", url, authorization, NULL, 0, &response, 5000) != 0) {
		snprintf(error, error_cap, "%s", response.error[0] ? response.error : "Falha de rede");
		http_response_free(&response);
		return -1;
	}
	if (response.status != 200) {
		snprintf(error, error_cap, "Episodios: HTTP %d", response.status);
		http_response_free(&response);
		return -1;
	}
	JsonDoc doc;
	if (json_parse(&doc, response.body, response.length) != 0) {
		snprintf(error, error_cap, "Resposta invalida");
		http_response_free(&response);
		return -1;
	}
	const int count = parse_items(response.body, &doc, 0, list);
	json_free(&doc);
	http_response_free(&response);
	return count;
}

int jf_get_image(const JfConfig *config, const char *item_id, uint8_t **data, size_t *length, char *error,
	size_t error_cap)
{
	char url[1024];
	char authorization[512];
	jf_auth_header(config, authorization, sizeof authorization);
	snprintf(url, sizeof url, "%s/Items/%s/Images/Primary?maxHeight=420&quality=88", config->server, item_id);
	HttpResponse response;
	if (http_request("GET", url, authorization, NULL, 0, &response, 8000) != 0) {
		snprintf(error, error_cap, "%s", response.error[0] ? response.error : "Falha de rede");
		http_response_free(&response);
		return -1;
	}
	if (response.status != 200 || !response.length) {
		snprintf(error, error_cap, "Imagem: HTTP %d", response.status);
		http_response_free(&response);
		return -1;
	}
	*data = (uint8_t *)response.body;
	*length = response.length;
	response.body = NULL;
	response.length = 0;
	http_response_free(&response);
	return 0;
}

void jf_stream_url(const JfConfig *config, const char *item_id, const char *play_session_id, char *url, size_t cap)
{
	snprintf(url, cap,
		"%s/videos/%s/stream.mpeg?DeviceId=%s&PlaySessionId=%s&Static=false&Container=mpeg&VideoCodec=mpeg1video&"
		"AudioCodec=mp2&VideoBitrate=2500000&AudioBitrate=192000&AudioSampleRate=48000&AudioChannels=2&"
		"MaxWidth=640&MaxHeight=480&MaxFramerate=30",
		config->server, item_id, config->device_id, play_session_id);
}

void jf_list_free(JfList *list)
{
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}
