/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef VDP_PLAYER_AUDIO_H
#define VDP_PLAYER_AUDIO_H

#include <signal.h>
#include <stdint.h>

struct player_audio;

/* A file without an audio stream leaves *audio NULL. */
int player_audio_open(struct player_audio **audio, const char *file,
		      const char *pcm, int64_t video_origin_us);
int player_audio_tone(struct player_audio **audio, const char *pcm, int64_t duration_us);
int player_audio_prime(struct player_audio *audio, int64_t pts_us, int64_t lead_us,
		       const volatile sig_atomic_t *stop);
int player_audio_start(struct player_audio *audio);
int player_audio_clock(struct player_audio *audio, int64_t *pts_us);
int player_audio_finish(struct player_audio *audio, const volatile sig_atomic_t *stop);
void player_audio_flush(struct player_audio *audio);
void player_audio_close(struct player_audio **audio);

#endif
