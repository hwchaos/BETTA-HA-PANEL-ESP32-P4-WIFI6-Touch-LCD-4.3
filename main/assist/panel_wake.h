/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#pragma once

/* On-device wake word ("Okay Nabu") for HA Assist.
 *
 * While enabled, the panel keeps the microphones open and runs a
 * microWakeWord model (TensorFlow Lite Micro, the same models ESPHome uses)
 * on 10 ms slices of audio. A detection starts a normal Assist session; Home
 * Assistant then detects the end of the sentence. Nothing leaves the panel
 * before the wake word was heard.
 *
 * The microphones and the speaker share the I2S clocks, so listening pauses
 * while the speaker is taken (radio, spoken answer) and while a session runs.
 *
 * Models: "okay_nabu" is built in; further microWakeWord v2 models are
 * installed from the URL of their manifest (.json next to the .tflite) and
 * kept on the internal storage. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "settings/runtime_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_WAKE_BUILTIN_MODEL "okay_nabu"
#define PANEL_WAKE_MODELS_MAX 12

typedef struct {
    bool enabled;          /* setting (and Assist enabled) */
    bool loaded;           /* model ready */
    bool listening;        /* microphones open, model running */
    char model[RUNTIME_WAKE_MODEL_MAX_LEN];
    char wake_word[48];    /* display name from the manifest */
    char error[96];        /* why it is not listening, "" = fine */
    uint32_t detections;
    uint32_t last_detection_s; /* uptime in s, 0 = never */
    uint32_t invoke_us;    /* average model run per 30 ms of audio */
    uint8_t peak;          /* highest probability of the last ~3 s, 0..255 */
    int16_t level_db;      /* loudest 10 ms of the last ~3 s, dBFS (-96 = silence) */
} panel_wake_status_t;

typedef struct {
    char id[RUNTIME_WAKE_MODEL_MAX_LEN];
    char wake_word[48];
    bool builtin;
    float cutoff;
    int window;
    int arena;
} panel_wake_model_t;

esp_err_t panel_wake_init(const runtime_settings_t *settings);
void panel_wake_apply_settings(const runtime_settings_t *settings);
void panel_wake_get_status(panel_wake_status_t *out);

/* Built-in plus installed models; returns the count. */
int panel_wake_list_models(panel_wake_model_t *out, int max);

/* Downloads a microWakeWord v2 manifest and the model next to it and stores
 * both. `id_out` receives the model id (file name without .tflite). */
esp_err_t panel_wake_install(const char *manifest_url, char *id_out, size_t id_len, char *err, size_t err_len);

/* Removes an installed model (not the built-in one). */
esp_err_t panel_wake_delete(const char *id);

/* Runs 16 kHz mono PCM through the loaded model instead of the microphones
 * (diagnostics): reports the highest probability and whether the wake word
 * would have been detected. No Assist session is started. */
esp_err_t panel_wake_test(const int16_t *pcm, size_t samples, bool *detected, uint8_t *peak);

#ifdef __cplusplus
}
#endif
