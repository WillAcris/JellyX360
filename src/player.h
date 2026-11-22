#ifndef JF_PLAYER_H
#define JF_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Player Player;

// Opens the MPEG-PS stream, starts the decoder and the audio feeder. NULL on failure, with error set.
// `headers` is an optional extra header block (the Authorization line) terminated with CRLF.
Player *player_start(const char *url, const char *headers, char *error, size_t error_cap);
void player_stop(Player *player);

// The newest decoded frame due at the audio clock, as 0xAARRGGBB pixels; NULL until one is ready. serial changes
// whenever a new frame was copied. The pointer stays valid until the next call.
const uint8_t *player_frame(Player *player, int *width, int *height, uint32_t *serial);
// True once the stream ended and every decoded frame has been handed to player_frame.
bool player_drained(Player *player);
bool player_failed(Player *player);
bool player_paused(Player *player);
void player_pause(Player *player, bool paused);
const char *player_error(Player *player);
double player_clock(Player *player);

#endif
