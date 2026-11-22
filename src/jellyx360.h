#ifndef JF_JELLYX360_H
#define JF_JELLYX360_H

#include "jf.h"

int jf_login(JfConfig *config, const char *server, const char *user, const char *password, char *error,
	size_t error_cap);
int jf_get_views(const JfConfig *config, JfList *list, char *error, size_t error_cap);
int jf_get_children(const JfConfig *config, const char *parent_id, const char *include_types, JfList *list, char *error, size_t error_cap);
int jf_get_resume(const JfConfig *config, JfList *list, char *error, size_t error_cap);
int jf_get_item(const JfConfig *config, const char *item_id, JfItem *item, char *error, size_t error_cap);
int jf_get_live_channels(const JfConfig *config, JfList *list, char *error, size_t error_cap);
int jf_get_episodes(const JfConfig *config, const char *series_id, JfList *list, char *error, size_t error_cap);
int jf_get_image(const JfConfig *config, const char *item_id, uint8_t **data, size_t *length, char *error,
	size_t error_cap);
void jf_stream_url(const JfConfig *config, const char *item_id, const char *play_session_id, char *url, size_t cap);
void jf_auth_header(const JfConfig *config, char *out, size_t cap);
void jf_list_free(JfList *list);

#endif