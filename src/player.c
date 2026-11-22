#include "player.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "jf.h"
#include "xsdk/audio.h"
#include "xsdk/log.h"

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

enum {
	VIDEO_SLOTS = 6,
	AUDIO_SECONDS = 3,
};

typedef struct {
	uint8_t *pixels;
	int width;
	int height;
	double time;
	bool ready;
} VideoSlot;

struct Player {
	volatile int stop;
	volatile int ended;
	volatile int failed;
	volatile int paused;
	volatile int audio_enabled;
	bool audio_open;
	HttpStream *stream;
	plm_t *plm;
	pthread_t decode_thread;
	pthread_t audio_thread;
	bool decode_started;
	bool audio_started;
	pthread_mutex_t lock;
	pthread_cond_t cond;

	VideoSlot slots[VIDEO_SLOTS];
	int head;
	int tail;
	int count;

	uint8_t *front;
	int front_width;
	int front_height;
	bool front_ready;
	uint32_t serial;

	// Interleaved stereo floats at the decoder's rate, in frames.
	float *ring;
	size_t ring_frames;
	size_t ring_write;
	double ring_read;
	double sample_ratio;

	// The clock starts at the first video frame: its PTS, and the audio or wall clock when it arrived.
	bool clock_ready;
	bool video_started;
	double clock_pts;
	uint32_t clock_played;
	uint64_t clock_ms;
	double paused_clock;
	uint64_t pause_time_ms;

	uint64_t start_ms;
	char error[256];
};

static void set_error(Player *player, const char *text)
{
	pthread_mutex_lock(&player->lock);
	jf_strlcpy(player->error, text, sizeof player->error);
	player->failed = 1;
	pthread_cond_broadcast(&player->cond);
	pthread_mutex_unlock(&player->lock);
}

static uint32_t audio_played(void)
{
	XsdkAudioStats stats;
	xsdk_audio_stats(&stats);
	return stats.played;
}

// Fills the demuxer's buffer from the socket. pl_mpeg asks for as much as fits, and a stream is never rewound.
static void stream_load(plm_buffer_t *self, void *user)
{
	Player *player = user;
	plm_buffer_discard_read_bytes(self);
	const size_t space = self->capacity - self->length;
	if (!space) {
		self->has_ended = 1;
		return;
	}
	size_t done = 0;
	while (done < space && !player->stop) {
		const int got = http_stream_read(player->stream, self->bytes + self->length + done, space - done);
		if (got < 0) {
			set_error(player, "Falha de rede durante o video");
			break;
		}
		if (got == 0)
			break;
		done += (size_t)got;
	}
	self->length += done;
	if (!done)
		self->has_ended = 1;
}

static void on_video(plm_t *plm, plm_frame_t *frame, void *user)
{
	(void)plm;
	Player *player = user;
	pthread_mutex_lock(&player->lock);
	while (!player->stop && player->count == VIDEO_SLOTS)
		pthread_cond_wait(&player->cond, &player->lock);
	if (player->stop) {
		pthread_mutex_unlock(&player->lock);
		return;
	}
	VideoSlot *const slot = &player->slots[player->tail];
	const size_t bytes = (size_t)frame->width * frame->height * 4;
	uint8_t *grown = realloc(slot->pixels, bytes);
	if (!grown) {
		pthread_mutex_unlock(&player->lock);
		set_error(player, "Sem memoria para o video");
		return;
	}
	slot->pixels = grown;
	slot->width = (int)frame->width;
	slot->height = (int)frame->height;
	slot->time = frame->time;
	slot->ready = true;
	// pl_mpeg writes A, R, G and B with the alpha byte left alone: 0xFF makes the pixels opaque 0xAARRGGBB.
	memset(slot->pixels, 0xFF, bytes);
	plm_frame_to_argb(frame, slot->pixels, slot->width * 4);
	if (!player->video_started) {
		player->video_started = true;
		player->clock_ready = true;
		player->clock_pts = frame->time;
		player->clock_played = player->audio_enabled ? audio_played() : 0;
		player->clock_ms = jf_time_ms();
		pthread_cond_broadcast(&player->cond);
	}
	player->tail = (player->tail + 1) % VIDEO_SLOTS;
	player->count++;
	pthread_cond_broadcast(&player->cond);
	pthread_mutex_unlock(&player->lock);
}

static void on_audio(plm_t *plm, plm_samples_t *samples, void *user)
{
	(void)plm;
	Player *player = user;
	if (!player->audio_enabled)
		return;
	pthread_mutex_lock(&player->lock);
	if (player->sample_ratio == 0.0) {
		const int rate = plm_get_samplerate(plm);
		player->sample_ratio = rate > 0 ? (double)rate / (double)XSDK_AUDIO_RATE_HZ : 1.0;
		jf_log("player: audio a %d Hz", rate);
	}
	for (unsigned i = 0; i < samples->count && !player->stop; i++) {
		// ring_read is a double (resampling). Casting it to size_t can underflow and deadlock the decoder
		// when the server delivers a sample rate above 48 kHz, so compare in double precision.
		while (!player->stop && (double)player->ring_write - player->ring_read + 1.0 > (double)player->ring_frames)
			pthread_cond_wait(&player->cond, &player->lock);
		if (player->stop)
			break;
		const size_t index = (player->ring_write % player->ring_frames) * 2;
		player->ring[index] = samples->interleaved[i * 2];
		player->ring[index + 1] = samples->interleaved[i * 2 + 1];
		player->ring_write++;
	}
	pthread_cond_broadcast(&player->cond);
	pthread_mutex_unlock(&player->lock);
}

static void *decode_main(void *argument)
{
	Player *player = argument;
	while (!player->stop) {
		pthread_mutex_lock(&player->lock);
		while (!player->stop && player->paused)
			pthread_cond_wait(&player->cond, &player->lock);
		pthread_mutex_unlock(&player->lock);
		if (player->stop)
			break;
		if (plm_has_ended(player->plm))
			break;
		plm_decode(player->plm, 0.05);
	}
	pthread_mutex_lock(&player->lock);
	player->ended = 1;
	pthread_cond_broadcast(&player->cond);
	pthread_mutex_unlock(&player->lock);
	return NULL;
}

static void *audio_main(void *argument)
{
	Player *player = argument;
	pthread_mutex_lock(&player->lock);
	while (!player->stop && !player->video_started)
		pthread_cond_wait(&player->cond, &player->lock);
	pthread_mutex_unlock(&player->lock);
	while (!player->stop) {
		float *frame = NULL;
		const NTSTATUS status = xsdk_audio_next_frame(500, &frame);
		if (status == STATUS_INVALID_HANDLE)
			break;
		if (!NT_SUCCESS(status) || !frame)
			continue;
		memset(frame, 0, XSDK_AUDIO_FRAME_FLOATS * sizeof(float));
		pthread_mutex_lock(&player->lock);
		if (!player->paused) {
			for (uint32_t i = 0; i < XSDK_AUDIO_FRAME_SAMPLES && !player->stop; i++) {
				const size_t read = (size_t)player->ring_read;
				if (read >= player->ring_write)
					break;
				const size_t index0 = (read % player->ring_frames) * 2;
				float left = player->ring[index0];
				float right = player->ring[index0 + 1];
				const double fraction = player->ring_read - (double)read;
				if (read + 1 < player->ring_write && fraction > 0.0) {
					const size_t index1 = ((read + 1) % player->ring_frames) * 2;
					left += (player->ring[index1] - left) * (float)fraction;
					right += (player->ring[index1 + 1] - right) * (float)fraction;
				}
				frame[XSDK_AUDIO_FRONT_LEFT * XSDK_AUDIO_FRAME_SAMPLES + i] = left;
				frame[XSDK_AUDIO_FRONT_RIGHT * XSDK_AUDIO_FRAME_SAMPLES + i] = right;
				frame[XSDK_AUDIO_CENTER * XSDK_AUDIO_FRAME_SAMPLES + i] = 0.5f * (left + right);
				player->ring_read += player->sample_ratio > 0.0 ? player->sample_ratio : 1.0;
			}
			pthread_cond_broadcast(&player->cond);
		}
		pthread_mutex_unlock(&player->lock);
		xsdk_audio_queue_frame();
	}
	return NULL;
}

Player *player_start(const char *url, const char *headers, char *error, size_t error_cap)
{
	Player *player = calloc(1, sizeof *player);
	if (!player) {
		jf_strlcpy(error, "Sem memoria", error_cap);
		return NULL;
	}
	pthread_mutex_init(&player->lock, NULL);
	pthread_cond_init(&player->cond, NULL);
	player->ring_frames = XSDK_AUDIO_RATE_HZ * AUDIO_SECONDS;
	player->ring = calloc(player->ring_frames * 2, sizeof *player->ring);
	player->start_ms = jf_time_ms();
	if (!player->ring) {
		jf_strlcpy(error, "Sem memoria para o audio", error_cap);
		goto fail;
	}

	jf_log("player: abrindo %s", url);
	player->stream = http_stream_open(url, headers, 8000);
	if (!player->stream) {
		jf_strlcpy(error, "Nao consegui abrir o fluxo de video", error_cap);
		goto fail;
	}
	const int status = http_stream_status(player->stream);
	if (status != 200) {
		snprintf(error, error_cap, "O servidor recusou o transcode (HTTP %d)", status);
		goto fail;
	}

	plm_buffer_t *buffer = plm_buffer_create_with_callbacks(stream_load, NULL, NULL, 0, player);
	if (!buffer) {
		jf_strlcpy(error, "Sem memoria para o buffer", error_cap);
		goto fail;
	}
	player->plm = plm_create_with_buffer(buffer, 1);
	if (!player->plm) {
		plm_buffer_destroy(buffer);
		jf_strlcpy(error, "Nao consegui criar o decodificador MPEG", error_cap);
		goto fail;
	}
	plm_set_video_enabled(player->plm, 1);
	plm_set_audio_enabled(player->plm, 1);
	plm_set_video_decode_callback(player->plm, on_video, player);
	plm_set_audio_decode_callback(player->plm, on_audio, player);

	if (NT_SUCCESS(xsdk_audio_open(XSDK_AUDIO_FRAMES_AHEAD_MAX))) {
		player->audio_open = true;
		player->audio_enabled = 1;
	} else {
		jf_log("player: audio indisponivel; seguindo sem som");
		plm_set_audio_enabled(player->plm, 0);
	}

	if (player->audio_enabled) {
		if (pthread_create(&player->audio_thread, NULL, audio_main, player) == 0)
			player->audio_started = true;
		else
			player->audio_enabled = 0;
	}
	if (pthread_create(&player->decode_thread, NULL, decode_main, player) == 0) {
		player->decode_started = true;
	} else {
		jf_strlcpy(error, "Nao consegui criar a thread de video", error_cap);
		goto fail;
	}
	jf_log("player: reproduzindo (%s)", player->audio_enabled ? "com audio" : "sem audio");
	return player;

fail:
	player_stop(player);
	return NULL;
}

void player_stop(Player *player)
{
	if (!player)
		return;
	player->stop = 1;
	pthread_mutex_lock(&player->lock);
	pthread_cond_broadcast(&player->cond);
	pthread_mutex_unlock(&player->lock);
	if (player->decode_started)
		pthread_join(player->decode_thread, NULL);
	if (player->audio_started)
		pthread_join(player->audio_thread, NULL);
	if (player->audio_open)
		xsdk_audio_close();
	if (player->plm)
		plm_destroy(player->plm);
	if (player->stream)
		http_stream_close(player->stream);
	for (int i = 0; i < VIDEO_SLOTS; i++)
		free(player->slots[i].pixels);
	free(player->front);
	free(player->ring);
	pthread_mutex_destroy(&player->lock);
	pthread_cond_destroy(&player->cond);
	free(player);
}

static double clock_now(Player *player)
{
	if (!player->clock_ready)
		return 0.0;
	if (player->paused)
		return player->paused_clock;
	if (player->audio_enabled) {
		const double played = (double)audio_played() * (double)XSDK_AUDIO_FRAME_SAMPLES / (double)XSDK_AUDIO_RATE_HZ;
		return player->clock_pts + played - (double)player->clock_played * (double)XSDK_AUDIO_FRAME_SAMPLES /
			(double)XSDK_AUDIO_RATE_HZ;
	}
	return player->clock_pts + (double)(jf_time_ms() - player->clock_ms) / 1000.0;
}

const uint8_t *player_frame(Player *player, int *width, int *height, uint32_t *serial)
{
	if (!player)
		return NULL;
	const double clock = clock_now(player);
	pthread_mutex_lock(&player->lock);
	while (player->count > 0) {
		VideoSlot *const slot = &player->slots[player->head];
		if (!slot->ready)
			break;
		if (player->count > 1 && slot->time > clock + 0.10)
			break;
		const size_t bytes = (size_t)slot->width * slot->height * 4;
		if (player->front_width != slot->width || player->front_height != slot->height) {
			uint8_t *grown = realloc(player->front, bytes);
			if (!grown)
				break;
			player->front = grown;
			player->front_width = slot->width;
			player->front_height = slot->height;
		}
		memcpy(player->front, slot->pixels, bytes);
		player->front_ready = true;
		player->serial++;
		player->head = (player->head + 1) % VIDEO_SLOTS;
		player->count--;
	}
	pthread_cond_broadcast(&player->cond);
	const bool ready = player->front_ready;
	if (width)
		*width = player->front_width;
	if (height)
		*height = player->front_height;
	if (serial)
		*serial = player->serial;
	pthread_mutex_unlock(&player->lock);
	return ready ? player->front : NULL;
}

bool player_drained(Player *player)
{
	if (!player || !player->ended)
		return false;
	pthread_mutex_lock(&player->lock);
	const bool drained = player->count == 0;
	pthread_mutex_unlock(&player->lock);
	return drained;
}

bool player_failed(Player *player)
{
	return player && player->failed;
}

const char *player_error(Player *player)
{
	return player ? player->error : "";
}

double player_clock(Player *player)
{
	return player ? clock_now(player) : 0.0;
}

bool player_paused(Player *player)
{
	return player ? (player->paused != 0) : false;
}

void player_pause(Player *player, bool paused)
{
	if (!player)
		return;
	pthread_mutex_lock(&player->lock);
	if (paused && !player->paused) {
		player->paused_clock = clock_now(player);
		player->paused = 1;
	} else if (!paused && player->paused) {
		player->paused = 0;
	}
	pthread_cond_broadcast(&player->cond);
	pthread_mutex_unlock(&player->lock);
}
