#pragma once

/* Home Assistant Assist (push-to-talk voice control).
 *
 * The panel records 16 kHz mono PCM from its microphones and streams it into
 * an assist_pipeline/run on the existing HA websocket (STT -> intent -> TTS).
 * Home Assistant does the speech recognition (e.g. Whisper / Speech-to-Phrase),
 * runs the command and renders the spoken answer (e.g. Piper), which the panel
 * plays through the on-board speaker. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "settings/runtime_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PANEL_ASSIST_IDLE = 0,
    PANEL_ASSIST_LISTENING,  /* microphone open, audio is streamed */
    PANEL_ASSIST_PROCESSING, /* waiting for recognition / intent / TTS */
    PANEL_ASSIST_DONE,       /* answer available (and spoken, if enabled) */
    PANEL_ASSIST_ERROR,
} panel_assist_state_t;

typedef struct {
    panel_assist_state_t state;
    uint32_t seq;        /* bumped on every change */
    char heard[160];     /* recognised text */
    char reply[320];     /* spoken answer as text */
    char error[96];      /* error code / message */
} panel_assist_status_t;

esp_err_t panel_assist_init(const runtime_settings_t *settings);

/* Takes over enabled / pipeline / reply audio / mic gain / max duration. */
void panel_assist_apply_settings(const runtime_settings_t *settings);

bool panel_assist_is_enabled(void);

/* Starts listening. While a session is busy the call is ignored. */
esp_err_t panel_assist_start(void);

/* Ends the recording (button released); the pipeline then finishes. */
void panel_assist_stop_listening(void);

/* Aborts the current session and hides it. */
void panel_assist_cancel(void);

bool panel_assist_is_busy(void);

void panel_assist_get_status(panel_assist_status_t *out);

#ifdef __cplusplus
}
#endif
