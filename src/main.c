#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "jellyx360.h"
#include "jf.h"
#include "player.h"
#include "style.h"
#include "ui.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/net.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/text.h"
#include "xsdk/texture.h"
#include "xsdk/xam.h"

// XAM starts with 32 sockets and 16 KB buffers; 63 KB buffers carry a transcode without starving the decoder.
const XsdkNetworkLimits xsdk_network_limits = {
	.stream_sockets = 64,
	.datagram_sockets = 16,
	.receive_kb = 63,
	.send_kb = 63,
};

enum {
	POSTER_SLOTS = 20,
	POSTER_QUEUE = 8,
	BEGIN_TIMEOUT_MS = 5000,
};

typedef enum {
	SCREEN_LOGIN,
	SCREEN_HOME,
	SCREEN_FAVORITES,
	SCREEN_DETAILS,
	SCREEN_PLAYER,
} Screen;

typedef struct {
	char id[64];
	bool valid;
	uint64_t used;
	XsdkTexture texture;
} Poster;

// A cover download runs on a worker thread. The main thread only decodes and uploads the
// texture, so a slow server can never stall the controller.
typedef struct {
	char id[64];
	uint8_t *data;
	size_t length;
	int state; // 0 = queued, 1 = ready
} PosterJob;

typedef struct {
	XsdkTexture texture;
	bool valid;
	int width;
	int height;
	uint32_t fence;
	bool fence_valid;
} VideoTexture;

typedef struct {
	bool held;
	uint64_t next;
} Repeat;
typedef struct HomeRow {
	char title[64];
	JfList items;
	int selected;
	int scroll;
} HomeRow;

typedef struct {
	Ui ui;
	uint32_t width;
	uint32_t height;
	Screen screen;
	JfConfig config;
	bool quit;

	JfList views;
	HomeRow home_rows[6];
	int home_row_count;
	int home_load_step;
	int home_row;

	// Grid (Movies/Series)
	JfList grid_items;
	char grid_title[64];
	char grid_parent_id[64]; // series id when the grid shows episodes
	int grid_selected;
	int grid_scroll;
	int grid_loaded;
	int active_tab;
	Repeat up;
	Repeat down;
	Repeat left;
	Repeat right;
	bool a_held;
	bool b_held;
	bool x_held;
	bool lb_held;
	bool rb_held;
	bool start_held;

	char message[512];
	uint64_t message_until;

	// Player OSD
	uint64_t osd_until;

	// On-screen keyboard
	bool keyboard_open;
	XAM_OVERLAPPED keyboard_overlapped;
	uint16_t keyboard_text[256];
	uint16_t keyboard_title[64];
	uint16_t keyboard_description[128];
	uint16_t keyboard_default[256];
	int keyboard_field;

	// Login
	char server[256];
	char user[64];
	char password[128];
	int field;

	// Posters
	Poster posters[POSTER_SLOTS];
	uint64_t poster_tick;
	PosterJob poster_jobs[POSTER_QUEUE];
	int poster_job_count;
	pthread_mutex_t poster_mutex;
	pthread_cond_t poster_cond;
	pthread_t poster_thread;
	bool poster_thread_started;
	bool poster_stop;
	JfConfig poster_config;

	// Player
	Player *player;
	char playing[192];
	char play_session_id[64];
	long long playing_ticks;

	// Details
	JfItem detail_item;
	VideoTexture video[2];
	int video_current;
	int video_next;
	uint32_t video_serial;
	bool video_any;
} App;

static void draw_frame(App *app);
static void draw_grid(App *app, const char *title);
static void update_grid(App *app);
static void set_message(App *app, const char *text);

static uint32_t read_buttons(void)
{
	INPUT_STATE state = { 0 };
	if (XamInputGetState(0, 0, &state) != ERROR_SUCCESS)
		return 0;
	return state.gamepad.buttons;
}

static bool repeat_now(Repeat *repeat, bool down, uint64_t now)
{
	if (!down) {
		repeat->held = false;
		return false;
	}
	if (!repeat->held) {
		repeat->held = true;
		repeat->next = now + 380;
		return true;
	}
	if (now >= repeat->next) {
		repeat->next = now + 130;
		return true;
	}
	return false;
}

static void set_message(App *app, const char *text)
{
	jf_strlcpy(app->message, text, sizeof app->message);
	app->message_until = jf_time_ms() + 6000;
	jf_log("app: %s", text);
}

static void draw_message(App *app)
{
	if (!app->message[0] || jf_time_ms() >= app->message_until)
		return;
	const float height = 60;
	const float y = (float)app->height - height - 24;
	ui_rect(&app->ui, UI_PAD_MD, y, (float)app->width - 2 * UI_PAD_MD, height, C_ALERT);
	char fitted[256];
	jf_fit_text(fitted, sizeof fitted, app->message, (int)(((float)app->width - 96) / 10));
	ui_text(2 * UI_PAD, y + 18, UI_SCALE_BODY, C_TEXT, fitted);
}

static void draw_footer(App *app, const char *hints)
{
	ui_rect(&app->ui, 0, (float)app->height - UI_FOOTER_H, (float)app->width, UI_FOOTER_H, C_PANEL);
	ui_text(UI_PAD_MD, (float)app->height - 27, UI_SCALE_BODY, C_DIM, hints);
}

// --- Posters ---------------------------------------------------------------------------------------------------

static Poster *poster_find(App *app, const char *id)
{
	if (!id[0])
		return NULL;
	for (int i = 0; i < POSTER_SLOTS; i++) {
		Poster *const poster = &app->posters[i];
		if (poster->id[0] && strcmp(poster->id, id) == 0) {
			poster->used = ++app->poster_tick;
			return poster;
		}
	}
	return NULL;
}

static void poster_queue(App *app, const char *id)
{
	pthread_mutex_lock(&app->poster_mutex);
	if (app->poster_job_count < POSTER_QUEUE) {
		PosterJob *job = &app->poster_jobs[app->poster_job_count++];
		memset(job, 0, sizeof *job);
		jf_strlcpy(job->id, id, sizeof job->id);
		pthread_cond_signal(&app->poster_cond);
	}
	pthread_mutex_unlock(&app->poster_mutex);
}

static bool poster_queued(App *app, const char *id)
{
	pthread_mutex_lock(&app->poster_mutex);
	bool found = false;
	for (int i = 0; i < app->poster_job_count; i++) {
		if (strcmp(app->poster_jobs[i].id, id) == 0) {
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&app->poster_mutex);
	return found;
}

static void *poster_thread_main(void *arg)
{
	App *app = arg;
	for (;;) {
		pthread_mutex_lock(&app->poster_mutex);
		int index = -1;
		for (int i = 0; i < app->poster_job_count; i++) {
			if (app->poster_jobs[i].state == 0) {
				index = i;
				break;
			}
		}
		while (index < 0 && !app->poster_stop) {
			pthread_cond_wait(&app->poster_cond, &app->poster_mutex);
			for (int i = 0; i < app->poster_job_count; i++) {
				if (app->poster_jobs[i].state == 0) {
					index = i;
					break;
				}
			}
		}
		if (index < 0) {
			pthread_mutex_unlock(&app->poster_mutex);
			break;
		}
		const PosterJob job = app->poster_jobs[index];
		const JfConfig config = app->poster_config;
		pthread_mutex_unlock(&app->poster_mutex);

		uint8_t *data = NULL;
		size_t length = 0;
		char error[256];
		if (jf_get_image(&config, job.id, &data, &length, error, sizeof error) != 0) {
			jf_log("poster: %s: %s", job.id, error);
			free(data);
			data = NULL;
			length = 0;
		}

		pthread_mutex_lock(&app->poster_mutex);
		app->poster_jobs[index].data = data;
		app->poster_jobs[index].length = length;
		app->poster_jobs[index].state = 1;
		pthread_mutex_unlock(&app->poster_mutex);
	}
	return NULL;
}

static void posters_pump(App *app)
{
	pthread_mutex_lock(&app->poster_mutex);
	PosterJob job;
	bool have = false;
	for (int i = 0; i < app->poster_job_count; i++) {
		if (app->poster_jobs[i].state != 1)
			continue;
		job = app->poster_jobs[i];
		memmove(&app->poster_jobs[i], &app->poster_jobs[i + 1],
			(size_t)(app->poster_job_count - i - 1) * sizeof job);
		app->poster_job_count--;
		have = true;
		break;
	}
	pthread_mutex_unlock(&app->poster_mutex);
	if (!have)
		return;

	Poster *slot = NULL;
	for (int i = 0; i < POSTER_SLOTS; i++) {
		if (!app->posters[i].id[0]) {
			slot = &app->posters[i];
			break;
		}
	}
	if (!slot) {
		slot = &app->posters[0];
		for (int i = 1; i < POSTER_SLOTS; i++)
			if (app->posters[i].used < slot->used)
				slot = &app->posters[i];
	}
	if (slot->id[0]) {
		xsdk_gpu_wait(xsdk_gpu_fence(), 500);
		if (slot->valid)
			xsdk_texture_free(&slot->texture);
	}
	slot->valid = false;
	jf_strlcpy(slot->id, job.id, sizeof slot->id);
	slot->used = ++app->poster_tick;

	if (job.data) {
		uint32_t *pixels = NULL;
		int width = 0;
		int height = 0;
		if (image_decode(job.data, job.length, &pixels, &width, &height) == 0) {
			if (NT_SUCCESS(xsdk_texture_create(&slot->texture, XSDK_FORMAT_A8R8G8B8,
					(uint32_t)width, (uint32_t)height, 1, true))) {
				xsdk_texture_upload(&slot->texture, 0, pixels, (uint32_t)width * 4);
				xsdk_gpu_invalidate(slot->texture.address, slot->texture.size_bytes);
				slot->valid = true;
			}
			image_free(pixels);
		}
		free(job.data);
	}
}

static void posters_prefetch(App *app, const JfList *list, int scroll, int visible)
{
	if (!list || visible <= 0 || list->count <= 0)
		return;
	int start = scroll;
	if (start < 0) start = 0;
	if (start >= list->count) start = list->count - 1;
	int end = start + visible;
	if (end > list->count)
		end = list->count;
	for (int i = start; i < end; i++) {
		if (poster_find(app, list->items[i].id) || poster_queued(app, list->items[i].id))
			continue;
		poster_queue(app, list->items[i].id);
		return;
	}
}

static void posters_update_config(App *app)
{
	pthread_mutex_lock(&app->poster_mutex);
	app->poster_config = app->config;
	pthread_mutex_unlock(&app->poster_mutex);
}

static void posters_clear_queue(App *app)
{
	pthread_mutex_lock(&app->poster_mutex);
	for (int i = 0; i < app->poster_job_count; i++)
		free(app->poster_jobs[i].data);
	app->poster_job_count = 0;
	pthread_mutex_unlock(&app->poster_mutex);
}

static void posters_start(App *app)
{
	pthread_mutex_init(&app->poster_mutex, NULL);
	pthread_cond_init(&app->poster_cond, NULL);
	posters_update_config(app);
	if (pthread_create(&app->poster_thread, NULL, poster_thread_main, app) == 0)
		app->poster_thread_started = true;
}

static void posters_free(App *app)
{
	pthread_mutex_lock(&app->poster_mutex);
	app->poster_stop = true;
	pthread_cond_broadcast(&app->poster_cond);
	pthread_mutex_unlock(&app->poster_mutex);
	if (app->poster_thread_started) {
		pthread_join(app->poster_thread, NULL);
		app->poster_thread_started = false;
	}
	for (int i = 0; i < app->poster_job_count; i++)
		free(app->poster_jobs[i].data);
	app->poster_job_count = 0;
	pthread_cond_destroy(&app->poster_cond);
	pthread_mutex_destroy(&app->poster_mutex);
	xsdk_gpu_wait(xsdk_gpu_fence(), 1000);
	for (int i = 0; i < POSTER_SLOTS; i++) {
		if (app->posters[i].valid)
			xsdk_texture_free(&app->posters[i].texture);
		app->posters[i].valid = false;
		app->posters[i].id[0] = '\0';
	}
}

// --- Text helpers ----------------------------------------------------------------------------------------------

static const char *type_name(const char *type)
{
	if (strcmp(type, "Movie") == 0)
		return "Filme";
	if (strcmp(type, "Episode") == 0)
		return "Episodio";
	if (strcmp(type, "Series") == 0)
		return "Serie";
	if (strcmp(type, "Season") == 0)
		return "Temporada";
	if (strcmp(type, "CollectionFolder") == 0)
		return "Biblioteca";
	if (strcmp(type, "BoxSet") == 0)
		return "Colecao";
	if (strcmp(type, "Folder") == 0)
		return "Pasta";
	if (strcmp(type, "MusicVideo") == 0)
		return "Clipe";
	if (strcmp(type, "Video") == 0)
		return "Video";
	return type;
}

static void format_duration(long long ticks, char *out, size_t cap)
{
	if (ticks <= 0) {
		jf_strlcpy(out, "", cap);
		return;
	}
	const long long minutes = ticks / 600000000;
	if (minutes >= 60)
		snprintf(out, cap, "%lldh %02lldmin", minutes / 60, minutes % 60);
	else
		snprintf(out, cap, "%lldmin", minutes);
}

static void format_time(double seconds, char *out, size_t cap)
{
	if (seconds < 0)
		seconds = 0;
	const int total = (int)seconds;
	snprintf(out, cap, "%d:%02d", total / 60, total % 60);
}

// --- Login -----------------------------------------------------------------------------------------------------

static void login_start_keyboard(App *app)
{
	static const char *const names[] = { "Servidor", "Usuario", "Senha" };
	jf_utf8_to_utf16(names[app->field], app->keyboard_title, sizeof app->keyboard_title / sizeof app->keyboard_title[0]);
	jf_utf8_to_utf16(app->field == 0 ? "Endereco do Jellyfin, como http://192.168.0.10:8096" :
		app->field == 1 ? "Usuario do Jellyfin" : "Senha do Jellyfin", app->keyboard_description,
		sizeof app->keyboard_description / sizeof app->keyboard_description[0]);
	const char *const current = app->field == 0 ? app->server : app->field == 1 ? app->user : app->password;
	jf_utf8_to_utf16(current, app->keyboard_default, sizeof app->keyboard_default / sizeof app->keyboard_default[0]);
	app->keyboard_text[0] = 0;

	// xsdk_xam_keyboard already prepares the overlapped; doing it here would leak the event it creates.
	uint32_t user_index = 0;
	char gamertag[XSDK_XAM_GAMERTAG_BYTES];
	// xsdk_xam_signed_in_user always writes to gamertag, so it must never be NULL.
	const uint32_t signed_in = xsdk_xam_signed_in_user(&user_index, gamertag);
	jf_log("login: signed_in=0x%08X user_index=%u", (unsigned)signed_in, (unsigned)user_index);
	const uint32_t result = xsdk_xam_keyboard(user_index, app->keyboard_title, app->keyboard_description,
		app->keyboard_default, app->keyboard_text, sizeof app->keyboard_text / sizeof app->keyboard_text[0],
		&app->keyboard_overlapped);
	jf_log("login: xsdk_xam_keyboard=0x%08X", (unsigned)result);
	if (result == ERROR_IO_PENDING) {
		app->keyboard_open = true;
		app->keyboard_field = app->field;
	} else if (result == ERROR_SUCCESS) {
		char text[256];
		jf_utf16_to_utf8(app->keyboard_text, text, sizeof text);
		if (app->field == 0) {
			jf_strlcpy(app->server, text, sizeof app->server);
			jf_trim(app->server);
		} else if (app->field == 1) {
			jf_strlcpy(app->user, text, sizeof app->user);
			jf_trim(app->user);
		} else {
			jf_strlcpy(app->password, text, sizeof app->password);
		}
	}
}

static void load_views(App *app);

static void login_submit(App *app)
{
	char error[256];
	set_message(app, "Entrando...");
	draw_frame(app);
	if (jf_login(&app->config, app->server, app->user, app->password, error, sizeof error) != 0) {
		set_message(app, error);
		return;
	}
	jf_config_save(&app->config);
	posters_update_config(app);
	app->password[0] = '\0';
	draw_frame(app);
	load_views(app);
}

static void draw_login(App *app)
{
	ui_rect_gradient(&app->ui, 0, 0, (float)app->width, (float)app->height, C_BG_TOP, C_BG_TOP, C_BG_BOTTOM, C_BG_BOTTOM);
	// Left identity panel.
	ui_rect_gradient(&app->ui, 0, 0, (float)app->width / 2, (float)app->height, C_PANEL_TOP, C_PANEL_TOP, C_PANEL_BOTTOM, C_PANEL_BOTTOM);
	ui_rect(&app->ui, (float)app->width / 2, 0, 2, (float)app->height, C_PANEL);
	ui_rect(&app->ui, 80, 160, 104, 6, C_ACCENT);
	// "Jelly" + "X360": two colours, one wordmark.
	ui_text(80, 188, UI_SCALE_TITLE, C_TEXT, "Jelly");
	ui_text(80 + 5 * 30, 188, UI_SCALE_TITLE, C_ACCENT, "X360");
	ui_text(80, 264, UI_SCALE_SECTION, C_TEXT, "Sua midia, no seu Xbox 360");
	ui_text(80, 312, UI_SCALE_BODY, C_DIM, "Conecte-se ao seu servidor Jellyfin");
	ui_text(80, 340, UI_SCALE_BODY, C_DIM, "para acessar filmes, series e TV ao vivo.");
	// Small feature cards give the empty side some structure.
	static const char *const features[] = { "FILMES", "SERIES", "TV AO VIVO" };
	for (int i = 0; i < 3; i++) {
		const float px = 80 + i * (UI_LOGIN_PILL_W + UI_LOGIN_PILL_GAP);
		ui_rect(&app->ui, px, 404, UI_LOGIN_PILL_W, UI_LOGIN_PILL_H, C_PANEL);
		ui_rect(&app->ui, px, 404, 4, UI_LOGIN_PILL_H, C_ACCENT);
		ui_text(px + 12, 414, UI_SCALE_BODY, C_DIM, features[i]);
	}
	// Right form panel.
	const float fx = (float)app->width / 2 + UI_PAD_MD + UI_PAD;
	ui_text(fx, 120, UI_SCALE_SECTION, C_TEXT, "Entrar");
	ui_rect(&app->ui, fx, 162, 72, 3, C_ACCENT);
	ui_text(fx, 176, UI_SCALE_BODY, C_DIM, "Endereco do servidor Jellyfin (HTTP, porta 8096).");

	static const char *const labels[] = { "Servidor", "Usuario", "Senha" };
	for (int i = 0; i < 3; i++) {
		const float y = 200 + i * UI_FIELD_GAP;
		const bool selected = app->field == i;
		ui_shadow(&app->ui, fx, y, UI_FIELD_W, UI_FIELD_H);
		// Border + fill: the selected card reads as a focused input at a glance.
		ui_rect(&app->ui, fx - 2, y - 2, UI_FIELD_W + 4, UI_FIELD_H + 4,
			selected ? C_ACCENT : C_PROGRESS_BG);
		ui_rect(&app->ui, fx, y, UI_FIELD_W, UI_FIELD_H, selected ? C_HOVER : C_PANEL);
		if (selected) {
			ui_rect(&app->ui, fx, y, UI_BORDER + 1, UI_FIELD_H, C_ACCENT);
			ui_rect(&app->ui, fx + UI_FIELD_W - 28, y + 24, 12, 24, C_ACCENT);
		}
		ui_text(fx + UI_FIELD_TEXT_X, y + 8, UI_SCALE_BODY,
			selected ? C_ACCENT : C_DIM, labels[i]);
		const char *const value = i == 0 ? app->server : i == 1 ? app->user : app->password;
		char fitted[256];
		if (value[0]) {
			const char *shown = value;
			char masked[256];
			if (i == 2) {
				// Never draw the password in plain text on the TV.
				const size_t length = strlen(value);
				memset(masked, '*', length);
				masked[length] = '\0';
				shown = masked;
			}
			jf_fit_text(fitted, sizeof fitted, shown, 40);
			ui_text(fx + UI_FIELD_TEXT_X, y + 32, UI_SCALE_SECTION, C_TEXT, fitted);
		} else {
			ui_text(fx + UI_FIELD_TEXT_X, y + 32, UI_SCALE_SECTION, C_DIM, "(vazio)");
		}
	}
	ui_rect(&app->ui, fx - 12, 480, UI_FIELD_W + 24, 34, C_PANEL);
	ui_text(fx, 490, UI_SCALE_BODY, C_TEXT, "A: editar    Start: entrar    B: sair");
	ui_text(fx, 520, UI_SCALE_BODY, C_DIM, "Use http:// e a porta 8096. HTTPS nao suportado.");
	draw_footer(app, "JellyX360 " JF_VERSION);
}

static void update_login(App *app)
{
	if (app->keyboard_open) {
		const uint32_t status = xsdk_xam_wait(&app->keyboard_overlapped, 0, NULL);
		if (status != ERROR_IO_INCOMPLETE) {
			jf_log("login: keyboard done 0x%08X", (unsigned)status);
			if (status == ERROR_SUCCESS) {
				char text[256];
				jf_utf16_to_utf8(app->keyboard_text, text, sizeof text);
				if (app->keyboard_field == 0) {
					jf_strlcpy(app->server, text, sizeof app->server);
					jf_trim(app->server);
				} else if (app->keyboard_field == 1) {
					jf_strlcpy(app->user, text, sizeof app->user);
					jf_trim(app->user);
				} else {
					jf_strlcpy(app->password, text, sizeof app->password);
				}
			}
			app->keyboard_open = false;
		}
		// While the keyboard is open, don't process other input.
		return;
	}
	const uint32_t buttons = read_buttons();
	const uint64_t now = jf_time_ms();
	if (repeat_now(&app->up, (buttons & INPUT_GAMEPAD_DPAD_UP) != 0, now))
		app->field = (app->field + 2) % 3;
	if (repeat_now(&app->down, (buttons & INPUT_GAMEPAD_DPAD_DOWN) != 0, now))
		app->field = (app->field + 1) % 3;
	const bool a = (buttons & INPUT_GAMEPAD_A) != 0;
	if (a && !app->a_held)
		login_start_keyboard(app);
	app->a_held = a;
	const bool start = (buttons & INPUT_GAMEPAD_START) != 0;
	if (start && !app->start_held)
		login_submit(app);
	app->start_held = start;
}

// --- Browse ----------------------------------------------------------------------------------------------------

static void load_views(App *app)
{
	char error[256];
	set_message(app, "Carregando...");
	draw_frame(app);
	if (jf_get_views(&app->config, &app->views, error, sizeof error) < 0) {
		jf_log("load_views: jf_get_views falhou: %s", error);
		set_message(app, error);
		app->screen = SCREEN_LOGIN;
		return;
	}
	jf_log("load_views: %d views", app->views.count);
	// Initialize home loading (done incrementally in update_home to avoid watchdog)
	app->home_row_count = 0;
	app->home_row = 0;
	app->home_load_step = 0;
	app->screen = SCREEN_HOME;
}

// Jellyfin uses PlaySessionId to tie a seek to the running transcode. Without one, the server can
// reuse the old job, which is exactly "the video starts over" when the d-pad is pressed.
static void new_play_session(App *app)
{
	static unsigned sequence;
	snprintf(app->play_session_id, sizeof app->play_session_id,
		"%016llx-%04x", (unsigned long long)jf_time_ms(), sequence++);
}

static void play_item(App *app, const JfItem *item)
{
	char url[1024];
	new_play_session(app);
	jf_stream_url(&app->config, item->id, app->play_session_id, url, sizeof url);
	char headers[512];
	jf_auth_header(&app->config, headers, sizeof headers);
	set_message(app, "Abrindo o video...");
	draw_frame(app);
	char error[256];
	app->player = player_start(url, headers, error, sizeof error);
	if (!app->player) {
		set_message(app, error);
		return;
	}
	jf_strlcpy(app->playing, item->name, sizeof app->playing);
	app->playing_ticks = item->runtime_ticks;
	app->video_serial = 0;
	app->video_any = false;
	app->screen = SCREEN_PLAYER;
	app->osd_until = jf_time_ms() + 4000;
	app->message[0] = '\0';
}

static void logout(App *app)
{
	app->config.token[0] = '\0';
	app->config.user_id[0] = '\0';
	jf_config_save(&app->config);
	posters_clear_queue(app);
	posters_update_config(app);
	jf_list_free(&app->views);
	app->screen = SCREEN_LOGIN;
	app->field = 0;
	set_message(app, "Voce saiu da conta");
}

static bool home_row_visible(HomeRow *row, int tab)
{
	if (tab == 0) return true;
	if (row->items.count == 0) return false;
	const char *t = row->title;
	if (tab == 1) return strcmp(t, "Canais ao vivo") == 0;
	if (tab == 4) return strcmp(t, "Continuar assistindo") == 0;
	if (tab == 2) {
		if (strstr(t, "Filme") || strstr(t, "Movie")) return true;
		return strcmp(row->items.items[0].type, "Movie") == 0;
	}
	if (tab == 3) {
		if (strstr(t, "Seri") || strstr(t, "Serie")) return true;
		const char *ty = row->items.items[0].type;
		return strcmp(ty, "Series") == 0 || strcmp(ty, "Season") == 0 || strcmp(ty, "Episode") == 0;
	}
	return false;
}

static HomeRow *live_row(App *app)
{
	for (int r = 0; r < app->home_row_count; r++)
		if (home_row_visible(&app->home_rows[r], 1))
			return &app->home_rows[r];
	return NULL;
}

static void draw_live_grid(App *app)
{
	HomeRow *row = live_row(app);
	if (!row) {
		ui_text(UI_PAD_LG, UI_SECTION_Y, UI_SCALE_SECTION, C_DIM, "Nenhum canal encontrado...");
		draw_footer(app, "A: abrir   B: sair   X: atualizar   LB/RB: abas   ^v<>: navegar");
		draw_message(app);
		return;
	}
	ui_rect(&app->ui, UI_PAD_LG - UI_GAP, 132, UI_BORDER + 1, 22, C_ACCENT);
	ui_text(UI_PAD_LG, 124, UI_SCALE_SECTION, C_TEXT, row->title);

	const int cols = UI_LIVE_COLS;
	const int rows = UI_LIVE_ROWS;
	const float card_w = UI_LIVE_CARD_W;
	const float card_h = UI_LIVE_CARD_H;
	const float gap = UI_LIVE_GAP;
	const int visible = cols * rows;
	int start = (row->selected / visible) * visible;
	if (start < 0) start = 0;
	if (start >= row->items.count && row->items.count > 0)
		start = ((row->items.count - 1) / visible) * visible;
	row->scroll = start;

	for (int i = 0; i < visible && start + i < row->items.count; i++) {
		const int idx = start + i;
		const int c = i % cols;
		const int r = i / cols;
		const float x = UI_PAD_LG + c * (card_w + gap);
		const float y = UI_LIVE_START_Y + r * (card_h + 42);
		const bool sel = idx == row->selected;
		Poster *poster = poster_find(app, row->items.items[idx].id);
		ui_shadow(&app->ui, x, y, card_w, card_h);
		if (poster && poster->valid)
			ui_quad(&app->ui, &poster->texture, x, y, card_w, card_h, 0xFFFFFFFF);
		else
			ui_rect(&app->ui, x, y, card_w, card_h, C_PLACEHOLDER);
		// Draw the frame after the cover so it is never painted over.
		const float border = sel ? 4 : 2;
		const uint32_t border_color = sel ? C_ACCENT : C_PLACEHOLDER;
		ui_rect(&app->ui, x - border, y - border, card_w + 2 * border, border, border_color);
		ui_rect(&app->ui, x - border, y + card_h, card_w + 2 * border, border, border_color);
		ui_rect(&app->ui, x - border, y, border, card_h, border_color);
		ui_rect(&app->ui, x + card_w, y, border, card_h, border_color);
		char fitted[24];
		jf_fit_text(fitted, sizeof fitted, row->items.items[idx].name, (int)(card_w / 10));
		if (sel)
			ui_rect(&app->ui, x, y, card_w, 22, C_SCRIM);
		ui_text(x + 4, y + (sel ? 2 : card_h + 4), UI_SCALE_BODY,
			sel ? C_TEXT : C_DIM, fitted);
	}
	draw_footer(app, "A: sintonizar   B: sair   X: atualizar   LB/RB: abas   ^v<>: navegar");
	draw_message(app);
}

static void draw_home(App *app)
{
	/* Background gradient */
	ui_rect_gradient(&app->ui, 0, 0, (float)app->width, (float)app->height,
		C_BG_TOP, C_BG_TOP, C_BG, C_BG);

	/* Header (height 120) */
	ui_rect_gradient(&app->ui, 0, 0, (float)app->width, UI_HEADER_H, C_BG_TOP, C_BG_TOP, C_BG, C_BG);
	ui_rect(&app->ui, 0, UI_HEADER_H - 2, (float)app->width, 2, C_PANEL);
	// Logo: Jelly + X360 (highlighted)
	ui_text(UI_PAD_LG, 38, UI_SCALE_SECTION, C_TEXT, "Jelly");
	ui_text(UI_LOGO_X360_X, 38, UI_SCALE_SECTION, C_ACCENT, "X360");
	// Nav tabs
	const char *tabs[] = { "Inicio", "TV ao Vivo", "Filmes", "Series" };
	float tx = UI_TAB_X;
	for (int i = 0; i < 4; i++) {
		const bool active = i == app->active_tab;
		const float tw = (float)strlen(tabs[i]) * UI_TAB_CHAR_W;
		if (active)
			ui_rect(&app->ui, tx - 12, 38, tw + 24, 48, C_HOVER);
		ui_text(tx, 48, UI_SCALE_SECTION, active ? C_TEXT : C_DIM, tabs[i]);
		if (active)
			ui_rect(&app->ui, tx, 82, tw, UI_BORDER, C_ACCENT);
		tx += tw + UI_TAB_GAP;
	}
	// Right actions: profile avatar
	const float ax = (float)app->width - UI_PAD_LG - 2;
	ui_rect(&app->ui, ax, 34, UI_AVATAR + 4, UI_AVATAR + 4, C_ACCENT);
	ui_rect(&app->ui, ax + 2, 36, UI_AVATAR, UI_AVATAR, C_PANEL);
	ui_text(ax + 10, 44, UI_SCALE_SECTION, C_TEXT, "U");

	if (app->active_tab == 1) {
		draw_live_grid(app);
		return;
	}

	/* Content sections (no hero banner, more rows) */
	if (app->home_row_count == 0)
		ui_text(UI_PAD_LG, UI_SECTION_Y, UI_SCALE_SECTION, C_DIM, "Carregando ou vazio...");
	const float section_y = UI_SECTION_Y;
	const float row_h = UI_ROW_H;
	const float poster_w = UI_CARD_W;
	const float poster_h = UI_CARD_H;
	const float gap = UI_GAP;
	float y = section_y;

	for (int r = 0; r < app->home_row_count; r++) {
		if (y + row_h > app->height - 40) break;
		if (!home_row_visible(&app->home_rows[r], app->active_tab)) continue;
		HomeRow *row = &app->home_rows[r];
		// Section title
		ui_rect(&app->ui, UI_PAD_LG - UI_GAP, y + 8, UI_BORDER + 1, 22, C_ACCENT);
		ui_text(UI_PAD_LG, y, UI_SCALE_SECTION, C_TEXT, row->title);
		if (row->items.count > 5) {
			ui_text((float)app->width - 160, y + 8, UI_SCALE_BODY, C_DIM, "Ver tudo");
		}
		// Cards
		const int visible = (int)((app->width - 112) / (poster_w + gap));
		int start = row->scroll;
		if (row->selected >= start + visible) start = row->selected - visible + 1;
		if (row->selected < start) start = row->selected;
		row->scroll = start;
		for (int i = 0; i < visible && start + i < row->items.count; i++) {
			const int idx = start + i;
			const float x = UI_PAD_LG + i * (poster_w + gap);
			// 48 px leaves room for the section title (40 px) plus a real gap.
			const float py = y + 48;
			const bool sel = (r == app->home_row && idx == row->selected);
			Poster *poster = poster_find(app, row->items.items[idx].id);
			ui_shadow(&app->ui, x, py, poster_w, poster_h);
			// A thin frame around every card; the focused one turns accent-coloured.
			ui_rect(&app->ui, x - 2, py - 2, poster_w + 4, poster_h + 4,
				sel ? C_ACCENT : C_PLACEHOLDER);
			if (poster && poster->valid)
				ui_quad(&app->ui, &poster->texture, x, py, poster_w, poster_h, 0xFFFFFFFF);
			else
				ui_rect(&app->ui, x, py, poster_w, poster_h, C_PLACEHOLDER);
			// Progress bar for continue watching
			if (row->items.items[idx].resume_ticks > 0 && row->items.items[idx].runtime_ticks > 0) {
				const float progress = (float)row->items.items[idx].resume_ticks / (float)row->items.items[idx].runtime_ticks;
				ui_rect(&app->ui, x, py + poster_h - 8, poster_w, 8, C_PROGRESS_BG);
				if (progress > 0 && progress < 1)
					ui_rect(&app->ui, x, py + poster_h - 8, poster_w * progress, 8, C_PROGRESS_FILL);
			}
			// Focused title sits inside the card, so it never overlaps the next row.
			if (sel) {
				char fitted[40];
				jf_fit_text(fitted, sizeof fitted, row->items.items[idx].name, (int)(poster_w / 10));
				ui_rect(&app->ui, x, py, poster_w, 22, C_SCRIM);
				ui_text(x + 4, py + 2, UI_SCALE_BODY, C_TEXT, fitted);
			}
		}
		y += row_h;
	}
	draw_footer(app, "A: abrir   B: sair   X: recarregar   LB/RB: abas   ^v<>: navegar");
	draw_message(app);
}

static void update_home(App *app)
{
	if (app->home_load_step >= 0) {
		char error[256];
		jf_log("update_home: loading step=%d", app->home_load_step);
		if (app->home_load_step == 0) {
			HomeRow *r = &app->home_rows[app->home_row_count];
			jf_strlcpy(r->title, "Continuar assistindo", sizeof r->title);
			r->selected = 0; r->scroll = 0;
			jf_get_resume(&app->config, &r->items, error, sizeof error);
			jf_log("load_home: resume=%d err=%s", r->items.count, error);
			if (r->items.count > 0) app->home_row_count++;
			app->home_load_step = 1;
			return;
		}
		int v = app->home_load_step - 1;
		if (v < app->views.count && app->home_row_count < 6) {
			HomeRow *r = &app->home_rows[app->home_row_count];
			jf_strlcpy(r->title, app->views.items[v].name, sizeof r->title);
			r->selected = 0; r->scroll = 0;
			jf_get_children(&app->config, app->views.items[v].id, NULL, &r->items, error, sizeof error);
			jf_log("load_home: lib %d = %d", v, r->items.count);
			if (r->items.count > 0) app->home_row_count++;
		}
		app->home_load_step++;
		if (app->home_load_step - 1 >= app->views.count) {
			if (app->home_row_count < 6) {
				HomeRow *r = &app->home_rows[app->home_row_count];
				jf_strlcpy(r->title, "Canais ao vivo", sizeof r->title);
				r->selected = 0; r->scroll = 0;
				jf_get_live_channels(&app->config, &r->items, error, sizeof error);
				jf_log("load_home: live = %d", r->items.count);
				if (r->items.count > 0) app->home_row_count++;
			}
			app->home_load_step = -1;
			if (app->home_row_count == 0) {
				HomeRow *r = &app->home_rows[app->home_row_count++];
				jf_strlcpy(r->title, "Bibliotecas", sizeof r->title);
				r->selected = 0; r->scroll = 0;
				r->items.items = calloc((size_t)app->views.count, sizeof *r->items.items);
				r->items.capacity = app->views.count;
				r->items.count = app->views.count;
				for (int i = 0; i < app->views.count; i++)
					r->items.items[i] = app->views.items[i];
			}
		}
		return;
	}
	if (app->home_row_count == 0) return;
	if (app->home_row >= app->home_row_count) app->home_row = 0;
	// The live tab has a single row; make sure it is the one receiving input.
	if (app->active_tab == 1) {
		for (int r = 0; r < app->home_row_count; r++) {
			if (home_row_visible(&app->home_rows[r], 1)) {
				app->home_row = r;
				break;
			}
		}
	}

	const uint32_t buttons = read_buttons();
	const uint64_t now = jf_time_ms();
	// Prefetch posters for the items currently on screen. The live tab uses a bigger grid,
	// so it needs a bigger window here too; otherwise only the first few covers ever load.
	for (int r = 0; r < app->home_row_count; r++) {
		HomeRow *row = &app->home_rows[r];
		const int visible = (app->active_tab == 1 && home_row_visible(row, 1))
			? UI_LIVE_COLS * UI_LIVE_ROWS
			: (int)((app->width - 112) / (UI_CARD_W + UI_GAP));
		posters_prefetch(app, &row->items, row->scroll, visible);
	}

	// LB/RB: switch tabs
	const bool lb = (buttons & INPUT_GAMEPAD_LEFT_SHOULDER) != 0;
	const bool rb = (buttons & INPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
	if (lb && !app->lb_held) {
		int prev_tab = app->active_tab;
		app->active_tab = (app->active_tab + 3) % 4;
		int r = app->home_row;
		while (r >= 0 && r < app->home_row_count && !home_row_visible(&app->home_rows[r], app->active_tab)) r--;
		if (r < 0) { r = 0; while (r < app->home_row_count && !home_row_visible(&app->home_rows[r], app->active_tab)) r++; }
		if (r >= app->home_row_count) { app->active_tab = prev_tab; }
		else { app->home_row = r; }
	}
	app->lb_held = lb;
	if (rb && !app->rb_held) {
		int prev_tab = app->active_tab;
		app->active_tab = (app->active_tab + 1) % 4;
		int r = app->home_row;
		while (r < app->home_row_count && !home_row_visible(&app->home_rows[r], app->active_tab)) r++;
		if (r >= app->home_row_count) { r = 0; while (r < app->home_row_count && !home_row_visible(&app->home_rows[r], app->active_tab)) r++; }
		if (r >= app->home_row_count) { app->active_tab = prev_tab; }
		else { app->home_row = r; }
	}
	app->rb_held = rb;

	HomeRow *row = &app->home_rows[app->home_row];
	// On the live tab this row is a multi-line grid: up/down move between its lines.
	const bool live_grid = app->active_tab == 1 && home_row_visible(row, 1);
	if (repeat_now(&app->up, (buttons & INPUT_GAMEPAD_DPAD_UP) != 0, now)) {
		if (live_grid) {
			if (row->selected >= UI_LIVE_COLS) row->selected -= UI_LIVE_COLS;
		} else {
			int r = app->home_row - 1;
			while (r >= 0 && !home_row_visible(&app->home_rows[r], app->active_tab)) r--;
			if (r >= 0) app->home_row = r;
		}
	}
	if (repeat_now(&app->down, (buttons & INPUT_GAMEPAD_DPAD_DOWN) != 0, now)) {
		if (live_grid) {
			if (row->selected + UI_LIVE_COLS < row->items.count) row->selected += UI_LIVE_COLS;
		} else {
			int r = app->home_row + 1;
			while (r < app->home_row_count && !home_row_visible(&app->home_rows[r], app->active_tab)) r++;
			if (r < app->home_row_count) app->home_row = r;
		}
	}
	if (repeat_now(&app->left, (buttons & INPUT_GAMEPAD_DPAD_LEFT) != 0, now)) {
		if (row->selected > 0) row->selected--;
	}
	if (repeat_now(&app->right, (buttons & INPUT_GAMEPAD_DPAD_RIGHT) != 0, now)) {
		if (row->selected + 1 < row->items.count) row->selected++;
	}
	if (live_grid)
		row->scroll = (row->selected / (UI_LIVE_COLS * UI_LIVE_ROWS)) * UI_LIVE_COLS * UI_LIVE_ROWS;
	const bool a = (buttons & INPUT_GAMEPAD_A) != 0;
	const bool a_pressed = a && !app->a_held;
	// Set the hold state before changing screens: A is usually still held on the next frame,
	// and the new screen would otherwise treat it as a fresh press.
	app->a_held = a;
	if (a_pressed) {
		if (row->items.count > 0 && row->selected < row->items.count) {
			JfItem *it = &row->items.items[row->selected];
			if (it->playable) {
				play_item(app, it);
				return;
			}
			if (strcmp(it->type, "Series") == 0) {
				// Series: open seasons/episodes list
				char err[256];
				jf_get_episodes(&app->config, it->id, &app->grid_items, err, sizeof err);
				jf_strlcpy(app->grid_parent_id, it->id, sizeof app->grid_parent_id);
				app->grid_title[0] = '\0';
				jf_strlcpy(app->grid_title, it->name, sizeof app->grid_title);
				jf_strlcat(app->grid_title, " - Episodios", sizeof app->grid_title);
				app->grid_selected = 0;
				app->grid_scroll = 0;
				app->grid_loaded = 1;
				app->screen = SCREEN_FAVORITES; // reuse grid screen for episodes
				return;
			}
			// drill into folder
			app->detail_item = *it;
			char detail_error[256];
			if (jf_get_item(&app->config, it->id, &app->detail_item, detail_error, sizeof detail_error) != 0)
				jf_log("detalhes: %s", detail_error);
			app->screen = SCREEN_DETAILS;
		}
	}
	const bool b = (buttons & INPUT_GAMEPAD_B) != 0;
	if (b && !app->b_held) {
		logout(app);
	}
	app->b_held = b;
}

static void draw_details(App *app)
{
	ui_rect_gradient(&app->ui, 0, 0, (float)app->width, (float)app->height, C_BG_TOP, C_BG_TOP, C_BG_BOTTOM, C_BG_BOTTOM);
	// poster
	Poster *p = poster_find(app, app->detail_item.id);
	ui_rect(&app->ui, UI_PAD - 2, 78, UI_DETAIL_CARD_W + 4, UI_DETAIL_CARD_H + 4, C_PROGRESS_BG);
	if (p && p->valid)
		ui_quad(&app->ui, &p->texture, UI_PAD, 80, UI_DETAIL_CARD_W, UI_DETAIL_CARD_H, 0xFFFFFFFF);
	else
		ui_rect(&app->ui, UI_PAD, 80, UI_DETAIL_CARD_W, UI_DETAIL_CARD_H, C_PLACEHOLDER);
	// title
	ui_text(240, 80, UI_SCALE_SECTION, C_TEXT, app->detail_item.name);
	char line[128];
	char dur[32];
	format_duration(app->detail_item.runtime_ticks, dur, sizeof dur);
	snprintf(line, sizeof line, "%s%s%s", type_name(app->detail_item.type), dur[0] ? "  -  " : "", dur);
	ui_text(240, 120, UI_SCALE_BODY, C_DIM, line);
	ui_rect(&app->ui, 240, 146, 64, 3, C_ACCENT);
	if (app->detail_item.overview[0]) {
		char fitted[1024];
		jf_strlcpy(fitted, app->detail_item.overview, sizeof fitted);
		int y = 160;
		char *p2 = fitted;
		for (int i = 0; i < 6 && *p2; i++) {
			char chunk[96];
			jf_strlcpy(chunk, p2, sizeof chunk);
			if (strlen(p2) > 80) {
				int cut = 80;
				while (cut > 0 && p2[cut] != ' ') cut--;
				if (cut == 0) cut = 80;
				chunk[cut] = '\0';
				p2 += cut;
				while (*p2 == ' ') p2++;
			} else {
				p2 += strlen(p2);
			}
			ui_text(240, y, UI_SCALE_BODY, C_TEXT, chunk);
			y += 22;
		}
	}
	if (app->detail_item.playable) {
		// 240 px fits "Reproduzir" at scale 2 with an even margin on both sides.
		ui_shadow(&app->ui, 240, app->height - 120, 240, 56);
		ui_rect(&app->ui, 240, app->height - 120, 240, 56, C_ACCENT);
		ui_text(260, app->height - 104, UI_SCALE_SECTION, C_TEXT, app->detail_item.resume_ticks > 0 ? "Continuar" : "Reproduzir");
	}
	draw_footer(app, "A: reproduzir   B: voltar   X: atualizar");
	draw_message(app);
}

static void update_details(App *app)
{
	const uint32_t buttons = read_buttons();
	const bool a = (buttons & INPUT_GAMEPAD_A) != 0;
	const bool a_pressed = a && !app->a_held;
	app->a_held = a; // play_item() switches to the player screen; don't leak the press into it.
	if (a_pressed && app->detail_item.playable) {
		play_item(app, &app->detail_item);
		return;
	}
	const bool b = (buttons & INPUT_GAMEPAD_B) != 0;
	if (b && !app->b_held) {
		app->screen = SCREEN_HOME;
	}
	app->b_held = b;
}

static void stop_playback(App *app)
{
	bool failed = player_failed(app->player);
	char error[256];
	jf_strlcpy(error, player_error(app->player), sizeof error);
	player_stop(app->player);
	app->player = NULL;
	for (int i = 0; i < 2; i++) {
		// The last frame's draw may still be sampling the texture.
		if (app->video[i].fence_valid)
			xsdk_gpu_wait(app->video[i].fence, 1000);
		if (app->video[i].valid) {
			xsdk_texture_free(&app->video[i].texture);
			app->video[i].valid = false;
		}
		app->video[i].fence_valid = false;
	}
	app->video_any = false;
	app->screen = SCREEN_HOME;
	if (failed && error[0])
		set_message(app, error);
}

static VideoTexture *video_slot(App *app, int width, int height)
{
	VideoTexture *const slot = &app->video[app->video_next];
	if (slot->valid && (slot->width != width || slot->height != height)) {
		if (slot->fence_valid)
			xsdk_gpu_wait(slot->fence, 1000);
		xsdk_texture_free(&slot->texture);
		slot->valid = false;
	}
	if (!slot->valid) {
		if (!NT_SUCCESS(xsdk_texture_create(&slot->texture, XSDK_FORMAT_A8R8G8B8, (uint32_t)width, (uint32_t)height, 1,
				true)))
			return NULL;
		slot->valid = true;
		slot->width = width;
		slot->height = height;
		slot->fence_valid = false;
	}
	if (slot->fence_valid) {
		xsdk_gpu_wait(slot->fence, 1000);
		slot->fence_valid = false;
	}
	return slot;
}

static void draw_player(App *app)
{
	ui_rect(&app->ui, 0, 0, (float)app->width, (float)app->height, C_BLACK);

	int video_width = 0;
	int video_height = 0;
	uint32_t serial = 0;
	const uint8_t *pixels = player_frame(app->player, &video_width, &video_height, &serial);
	if (pixels && serial != app->video_serial) {
		VideoTexture *const slot = video_slot(app, video_width, video_height);
		if (slot) {
			xsdk_texture_upload(&slot->texture, 0, pixels, (uint32_t)video_width * 4);
			xsdk_gpu_invalidate(slot->texture.address, slot->texture.size_bytes);
			app->video_current = app->video_next;
			app->video_next ^= 1;
			app->video_serial = serial;
			app->video_any = true;
		}
	}
	if (app->video_any) {
		VideoTexture *const slot = &app->video[app->video_current];
		const float scale_x = (float)app->width / (float)slot->width;
		const float scale_y = (float)app->height / (float)slot->height;
		const float scale = scale_x < scale_y ? scale_x : scale_y;
		const float width = (float)slot->width * scale;
		const float height = (float)slot->height * scale;
		ui_quad(&app->ui, &slot->texture, ((float)app->width - width) / 2, ((float)app->height - height) / 2, width,
			height, 0xFFFFFFFF);
	}

	const bool osd = player_paused(app->player) || jf_time_ms() < app->osd_until;
	if (osd) {
		ui_rect(&app->ui, 0, 0, (float)app->width, UI_OSD_HEADER_H, C_SCRIM);
		ui_rect(&app->ui, 0, 0, UI_OSD_ACCENT_W, UI_OSD_HEADER_H, C_ACCENT);
		ui_rect(&app->ui, 0, UI_OSD_HEADER_H, (float)app->width, 2, C_PANEL);
		char fitted[256];
		jf_fit_text(fitted, sizeof fitted, app->playing, (int)(((float)app->width - 64) / 10));
		ui_text(UI_PAD, 12, UI_SCALE_BODY, C_TEXT, fitted);
		if (player_paused(app->player)) {
			ui_rect(&app->ui, UI_PAD - 8, 44, 88, 24, C_ACCENT);
			ui_text(UI_PAD, 48, UI_SCALE_BODY, C_TEXT, "PAUSADO");
		}
		char elapsed[32];
		char total[32];
		format_time(player_clock(app->player), elapsed, sizeof elapsed);
		format_duration(app->playing_ticks, total, sizeof total);
		char status[96];
		if (total[0])
			snprintf(status, sizeof status, "%s / %s", elapsed, total);
		else
			jf_strlcpy(status, elapsed, sizeof status);
		ui_text((float)app->width - UI_PAD - (float)strlen(status) * 10, 24, UI_SCALE_BODY, C_TEXT, status);

		// Progress bar
		if (app->playing_ticks > 0) {
			const float bar_y = (float)app->height - 64;
			const float bar_w = (float)app->width - 2 * UI_PAD;
			ui_rect(&app->ui, UI_PAD, bar_y, bar_w, UI_OSD_BAR_H, C_TRACK);
			const float progress = (float)(player_clock(app->player) / ((double)app->playing_ticks / 10000000.0));
			if (progress > 0 && progress < 1)
				ui_rect(&app->ui, UI_PAD, bar_y, bar_w * progress, UI_OSD_BAR_H, C_PROGRESS_FILL);
		}

		ui_rect(&app->ui, 0, (float)app->height - 40, (float)app->width, 40, C_SCRIM);
		ui_rect(&app->ui, 0, (float)app->height - 40, UI_OSD_ACCENT_W, 40, C_ACCENT);
		ui_text(UI_PAD, (float)app->height - 32, UI_SCALE_BODY, C_DIM, "A: pausa   B: parar");
	}
	draw_message(app);
}

static void update_player(App *app)
{
	if (player_failed(app->player) || player_drained(app->player)) {
		stop_playback(app);
		return;
	}
	const uint32_t buttons = read_buttons();
	const uint64_t now = jf_time_ms();
	// Show OSD on any input
	if (buttons) app->osd_until = now + 4000;

	const bool a = (buttons & INPUT_GAMEPAD_A) != 0;
	const bool a_pressed = a && !app->a_held;
	app->a_held = a;
	if (a_pressed) {
		player_pause(app->player, !player_paused(app->player));
		app->osd_until = now + 4000;
	}

	const bool b = (buttons & INPUT_GAMEPAD_B) != 0;
	if (b && !app->b_held) {
		app->b_held = b;
		stop_playback(app);
		return;
	}
	app->b_held = b;

	// Seeking is disabled for now: this server restarts the transcode instead of moving the
	// position. Keep the repeat state in sync so navigation still feels right after leaving.
	repeat_now(&app->left, (buttons & INPUT_GAMEPAD_DPAD_LEFT) != 0, now);
	repeat_now(&app->right, (buttons & INPUT_GAMEPAD_DPAD_RIGHT) != 0, now);
}

static void draw_grid(App *app, const char *title)
{
	ui_rect_gradient(&app->ui, 0, 0, (float)app->width, (float)app->height, C_BG_TOP, C_BG_TOP, C_BG_BOTTOM, C_BG_BOTTOM);
	ui_text(UI_PAD, 18, UI_SCALE_SECTION, C_ACCENT, title);
	ui_rect(&app->ui, UI_PAD, 58, 64, 3, C_ACCENT);
	if (app->grid_items.count == 0) {
		ui_text(2 * UI_PAD, 140, UI_SCALE_SECTION, C_DIM, "Carregando ou vazio...");
		draw_footer(app, "B: voltar");
		draw_message(app);
		return;
	}
	const int cols = UI_GRID_COLS;
	const float poster_w = UI_GRID_CARD_W;
	const float poster_h = UI_GRID_CARD_H;
	const float gap = UI_LIVE_GAP;
	const float start_x = UI_PAD;
	const float start_y = 70;
	const int visible_rows = UI_GRID_ROWS;
	int scroll_row = app->grid_scroll;
	int sel_row = app->grid_selected / cols;
	if (sel_row < scroll_row) scroll_row = sel_row;
	if (sel_row >= scroll_row + visible_rows) scroll_row = sel_row - visible_rows + 1;
	app->grid_scroll = scroll_row;
	for (int r = 0; r < visible_rows; r++) {
		for (int c = 0; c < cols; c++) {
			int idx = (scroll_row + r) * cols + c;
			if (idx >= app->grid_items.count) break;
			float x = start_x + c * (poster_w + gap);
			float y = start_y + r * UI_GRID_ROW_STEP;
			bool sel = (idx == app->grid_selected);
			Poster *pp = poster_find(app, app->grid_items.items[idx].id);
			ui_shadow(&app->ui, x, y, poster_w, poster_h);
			ui_rect(&app->ui, x - 2, y - 2, poster_w + 4, poster_h + 4,
				sel ? C_ACCENT : C_PLACEHOLDER);
			if (pp && pp->valid)
				ui_quad(&app->ui, &pp->texture, x, y, poster_w, poster_h, 0xFFFFFFFF);
			else
				ui_rect(&app->ui, x, y, poster_w, poster_h, C_PLACEHOLDER);
			char fitted[32];
			jf_fit_text(fitted, sizeof fitted, app->grid_items.items[idx].name, (int)(poster_w / 10));
			ui_text(x, y + poster_h + 4, UI_SCALE_BODY, sel ? C_TEXT : C_DIM, fitted);
		}
	}
	draw_footer(app, "A: abrir   B: voltar   X: atualizar   ^v<>: navegar");
	draw_message(app);
}

static void update_grid(App *app)
{
	if (!app->grid_loaded) {
		char err[256];
		if (app->grid_parent_id[0])
			jf_get_episodes(&app->config, app->grid_parent_id, &app->grid_items, err, sizeof err);
		app->grid_loaded = 1;
		return;
	}
	JfList *list = &app->grid_items;
	const uint32_t buttons = read_buttons();
	const uint64_t now = jf_time_ms();
	const int cols = UI_GRID_COLS;
	const int visible_rows = UI_GRID_ROWS;
	posters_prefetch(app, list, app->grid_scroll * cols, visible_rows * cols);
	if (list->count > 0) {
		if (repeat_now(&app->up, (buttons & INPUT_GAMEPAD_DPAD_UP) != 0, now)) {
			if (app->grid_selected >= cols) app->grid_selected -= cols;
		}
		if (repeat_now(&app->down, (buttons & INPUT_GAMEPAD_DPAD_DOWN) != 0, now)) {
			if (app->grid_selected + cols < list->count) app->grid_selected += cols;
		}
		if (repeat_now(&app->left, (buttons & INPUT_GAMEPAD_DPAD_LEFT) != 0, now)) {
			if (app->grid_selected > 0) app->grid_selected--;
		}
		if (repeat_now(&app->right, (buttons & INPUT_GAMEPAD_DPAD_RIGHT) != 0, now)) {
			if (app->grid_selected + 1 < list->count) app->grid_selected++;
		}
	}
	const bool a = (buttons & INPUT_GAMEPAD_A) != 0;
	if (a && !app->a_held && list->count > 0) {
		play_item(app, &list->items[app->grid_selected]);
	}
	app->a_held = a;
	const bool b = (buttons & INPUT_GAMEPAD_B) != 0;
	if (b && !app->b_held) {
		app->screen = SCREEN_HOME;
	}
	app->b_held = b;
}


// --- Frame loop ------------------------------------------------------------------------------------------------

static void draw_frame(App *app)
{
	if (xsdk_system_ui_open()) {
		uint32_t address = 0;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (NT_SUCCESS(status)) {
			xsdk_gpu_set_target(app->width, app->height);
			xsdk_gpu_resolve(address, app->width, app->height, C_BG);
			xsdk_present_end();
		}
		return;
	}
	uint32_t address = 0;
	const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		jf_log("app: xsdk_present_begin failed: 0x%08X", (unsigned)status);
		app->quit = true;
		return;
	}
	ui_begin(&app->ui, app->width, app->height);
	switch (app->screen) {
	case SCREEN_LOGIN:
		draw_login(app);
		break;
	case SCREEN_HOME:
		draw_home(app);
		break;
	case SCREEN_FAVORITES:
		draw_grid(app, app->grid_title);
		break;
	case SCREEN_DETAILS:
		draw_details(app);
		break;
	case SCREEN_PLAYER:
		draw_player(app);
		break;
	}
	xsdk_gpu_resolve(address, app->width, app->height, C_BG);
	ui_end(&app->ui);
	if (app->screen == SCREEN_PLAYER && app->video_any) {
		VideoTexture *const slot = &app->video[app->video_current];
		slot->fence = app->ui.fence[app->ui.slot];
		slot->fence_valid = true;
	}
	xsdk_present_end();
}

// X refreshes whichever list is on screen. It never changes the screen, so a failed
// request simply leaves the current list in place with the old data.
static void reload_current(App *app)
{
	switch (app->screen) {
	case SCREEN_HOME: {
		for (int i = 0; i < app->home_row_count; i++)
			jf_list_free(&app->home_rows[i].items);
		app->home_row_count = 0;
		app->home_row = 0;
		app->home_load_step = 0;
		break;
	}
	case SCREEN_FAVORITES:
		jf_list_free(&app->grid_items);
		app->grid_loaded = 0;
		app->grid_scroll = 0;
		app->grid_selected = 0;
		break;
	case SCREEN_DETAILS: {
		char id[64];
		jf_strlcpy(id, app->detail_item.id, sizeof id);
		if (id[0]) {
			char error[256];
			if (jf_get_item(&app->config, id, &app->detail_item, error, sizeof error) != 0)
				jf_log("detalhes: %s", error);
		}
		break;
	}
	default:
		return;
	}
	set_message(app, "Lista atualizada");
}

static void update(App *app)
{
	// While the XAM keyboard is open, let the screen handler poll the overlapped.
	// It returns early on its own; don't process global input or quit here.
	if (app->keyboard_open) {
		switch (app->screen) {
		case SCREEN_LOGIN:
			update_login(app);
			break;
		default:
			break;
		}
		return;
	}
	const uint32_t buttons = read_buttons();
	if (buttons & INPUT_GAMEPAD_BACK) {
		if (app->screen == SCREEN_PLAYER && app->player) {
			stop_playback(app);
			return;
		}
		if (app->screen == SCREEN_LOGIN) {
			app->quit = true;
			return;
		}
		// On other screens, ignore Back to avoid accidental exits.
		return;
	}
	const bool x = (buttons & INPUT_GAMEPAD_X) != 0;
	if (x && !app->x_held)
		reload_current(app);
	app->x_held = x;
	switch (app->screen) {
	case SCREEN_LOGIN:
		update_login(app);
		break;
	case SCREEN_HOME:
		update_home(app);
		break;
	case SCREEN_FAVORITES:
		update_grid(app);
		break;
	case SCREEN_DETAILS:
		update_details(app);
		break;
	case SCREEN_PLAYER:
		update_player(app);
		break;
	}
}

static void app_shutdown(App *app)
{
	if (app->player)
		player_stop(app->player);
	app->player = NULL;
	posters_free(app);
	jf_list_free(&app->views);
	xsdk_present_close();
	ui_shutdown(&app->ui);
	xsdk_text_close();
	xsdk_gpu_close();
}

int main(void)
{
	xsdk_log_write("jellyx360: Jellyfin para Xbox 360, " JF_VERSION);
	const NTSTATUS splash = xsdk_splash_show(1500);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("jellyx360: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("jellyx360: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	status = xsdk_text_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("jellyx360: xsdk_text_open failed: 0x%08X", (unsigned)status);
		xsdk_gpu_close();
		return 1;
	}

	static App app;
	jf_config_load(&app.config);
	jf_strlcpy(app.server, app.config.server, sizeof app.server);
	jf_strlcpy(app.user, app.config.user, sizeof app.user);
	app.field = 0;
	app.screen = SCREEN_LOGIN;
	if (ui_init(&app.ui) != 0) {
		xsdk_log_write("jellyx360: ui_init failed");
		xsdk_text_close();
		xsdk_gpu_close();
		return 1;
	}
	status = xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &app.width, &app.height);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("jellyx360: xsdk_present_open failed: 0x%08X", (unsigned)status);
		ui_shutdown(&app.ui);
		xsdk_text_close();
		xsdk_gpu_close();
		return 1;
	}
	xsdk_log_write("jellyx360: %ux%u, servidor %s", (unsigned)app.width, (unsigned)app.height,
		app.config.server[0] ? app.config.server : "(nao configurado)");

	posters_start(&app);
	if (app.config.token[0])
		load_views(&app);
	while (!app.quit) {
		posters_pump(&app);
		update(&app);
		if (app.quit)
			break;
		draw_frame(&app);
	}
	app_shutdown(&app);
	xsdk_log_write("jellyx360: encerrado");
	return 0;
}
