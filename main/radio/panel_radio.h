/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * On-panel internet radio (MP3/AAC stream -> ES8311 speaker).
 *
 * The radio page normally hands a station URL to a Home Assistant
 * media_player.  When the user picks "Głośnik panelu" the page calls into this
 * module instead and the panel plays the stream itself, so a station works
 * without any speaker in HA.
 *
 * The engine owns two worker tasks (a network task with the HTTP client and
 * the decoder, plus a playback task feeding I2S from a jitter buffer) and
 * borrows the speaker from the voice path for the duration of a stream.  It is
 * non-blocking: play()/stop() post a command, the caller polls the getters.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest stream URL / station name the engine keeps.  The radio page
 * validator limits station URLs to UI_RADIO_STATION_URL_LEN and names to
 * UI_RADIO_STATION_NAME_LEN, both below these. */
#define PANEL_RADIO_URL_MAX 192
#define PANEL_RADIO_TITLE_MAX 64
/* Longest error message panel_radio_get_error() returns. */
#define PANEL_RADIO_TEXT_MAX 64

typedef enum {
    PANEL_RADIO_IDLE = 0, /* nothing requested, or stopped */
    PANEL_RADIO_BUFFERING, /* connecting / waiting for the first frame */
    PANEL_RADIO_PLAYING,   /* audio is going to the speaker */
    PANEL_RADIO_ERROR,     /* station is not playable, see the error text */
} panel_radio_state_t;

/* Start (or replace) the stream.  Returns as soon as the command is queued;
 * ESP_ERR_NO_MEM when the worker task cannot be created.  `title` may be NULL
 * and is only used to report back what is playing. */
esp_err_t panel_radio_play(const char *url, const char *title);

/* Stop the stream and give the speaker back.  Safe to call when idle. */
void panel_radio_stop(void);

/* True while a stream is buffering or playing. */
bool panel_radio_is_active(void);

/* Current state; PANEL_RADIO_IDLE before the first play. */
panel_radio_state_t panel_radio_get_state(void);

/* Station title of the current/last stream; `out` is always NUL terminated. */
void panel_radio_get_title(char *out, size_t out_len);

/* Error text of the last PANEL_RADIO_ERROR; empty while the stream is fine. */
void panel_radio_get_error(char *out, size_t out_len);

/* Sample rate of the running stream, 0 when idle. */
uint32_t panel_radio_get_rate(void);

/* TEMPORARY (goes with the /api/radio debug route in api_routes.c): counters of
 * the decode loop.  The cumulative fields restart on every connection; the
 * per-second fields are refreshed by the 10 second statistic line together with
 * `window_s`. */
typedef struct {
    uint32_t samples;  /* mono frames handed to the jitter buffer */
    uint32_t calls;    /* decode calls */
    uint32_t ok;       /* calls that produced PCM */
    uint32_t decerr;   /* frames the decoder rejected */
    uint32_t resync;   /* forward walks after a rejection */
    uint32_t bytes_in; /* stream bytes read from the socket */
    uint32_t window_s; /* seconds the fields below were measured over */
    uint32_t samples_s;
    uint32_t ok_s;
    uint32_t calls_s;
    uint32_t err_s;
    uint32_t resync_s;
    uint32_t pcm_per_ok; /* bytes of PCM per successful call */
    uint32_t in_bps;
    uint32_t ring_kb;
    uint32_t rate;
} panel_radio_stats_t;

void panel_radio_get_stats(panel_radio_stats_t *out);

#ifdef __cplusplus
}
#endif
