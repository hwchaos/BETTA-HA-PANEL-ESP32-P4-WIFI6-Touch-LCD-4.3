#pragma once

// ============================================================
// Panel audio backend
// ============================================================
//
// Originally ported from ForgeUI 30_Audio.c for the BETTA-HA-PANEL
// firmware (it started out as the audio path of the Xiaozhi client).
//
// Responsibilities:
//
// - initialise the board BSP audio path (ES8311 codec via I2S)
// - manage speaker output volume
// - provide a simple speaker test beep
// - expose a duplex PCM path to the voice assistant (HA Assist)
//   and the speaker to the internet radio
//   (16 kHz / 16-bit; mic captured in stereo and downmixed to
//   mono by the caller)
//
// Rules:
//
// - backend owns audio truth
// - no LVGL ownership here
// - no UI styling here
//
// ============================================================

#include "esp_err.h"
#include "esp_codec_dev.h"

#include <stdbool.h>
#include <stdint.h>

/* The boards carry two physical MEMS mics on the ES7210 (MIC1+MIC2).
 * With exactly two mics selected the ES7210 runs in non-TDM stereo, so the
 * live mics can land on either I2S slot. Capture is therefore opened in
 * stereo and the caller downmixes the interleaved L/R data to mono. */
#define PANEL_AUDIO_MIC_CHANNELS 2

// ============================================================
// Audio API
// ============================================================

// Init audio system (called automatically on first use)
esp_err_t panel_audio_init(void);

// Set speaker volume (0-100)
esp_err_t panel_audio_set_volume(int volume);
// Get current speaker volume (0-100)
int panel_audio_get_volume(void);

// Play test beep
esp_err_t panel_audio_test_beep(void);


// ============================================================
// Duplex Pipeline API (voice assistant)
// ============================================================

// Open the microphone capture device (lazy; call when a session starts).
esp_err_t panel_audio_open_mic(void);

// Open the microphone for the wake word listener without stopping a radio
// stream: fails (ESP_ERR_INVALID_STATE) while the speaker is taken. The
// listener must close it again once panel_audio_output_wanted() turns true.
esp_err_t panel_audio_open_mic_shared(void);

// True while someone waits for a shared microphone to be closed.
bool panel_audio_output_wanted(void);

// Close the microphone capture device (call when a session ends).
esp_err_t panel_audio_close_mic(void);

// Set microphone gain in dB (ES7210), e.g. 24.0f.
esp_err_t panel_audio_set_mic_gain(float db);

// Play 16-bit mono PCM samples through the speaker.
esp_err_t panel_audio_play(const int16_t *pcm, int sample_count);

// Read up to max_frames 16-bit stereo frames (interleaved L/R) from the
// mic. Returns the number of frames read, or a negative value on error.
// On success the buffer holds frames_read * PANEL_AUDIO_MIC_CHANNELS samples; the
// caller must downmix to mono before Opus encoding.
int panel_audio_read(int16_t *pcm, int max_frames);

// Raw codec handles for advanced use (Opus, future DSP, etc.).
esp_codec_dev_handle_t panel_audio_get_speaker(void);
esp_codec_dev_handle_t panel_audio_get_mic(void);

// Sample info matching the I2S configuration (16 kHz / 16-bit).
// Mic capture is stereo (PANEL_AUDIO_MIC_CHANNELS); speaker output is mono.
esp_codec_dev_sample_info_t panel_audio_sample_info(void);
esp_codec_dev_sample_info_t panel_audio_mic_sample_info(void);


// ============================================================
// Exclusive Output API (on-panel internet radio)
// ============================================================
//
// The ES8311 speaker is a single resource shared with the voice path.
// The radio borrows it for the duration of a stream:
//
//   panel_audio_acquire_output(44100) -> ... writes via panel_audio_get_speaker()
//                                  -> panel_audio_release_output()
//
// While the output is exclusive the voice path refuses to start
// (panel_audio_play() returns ESP_ERR_INVALID_STATE) and panel_audio_open_mic()
// stops the stream first, because the microphones and the speaker share the
// I2S clocks.

// Take the speaker at `rate` Hz (mono / 16-bit).
// Fails with ESP_ERR_INVALID_STATE when the output is already taken or while
// the microphone is open.
esp_err_t panel_audio_acquire_output(uint32_t rate);

// Give the speaker back and restore the voice path format (16 kHz mono).
esp_err_t panel_audio_release_output(void);

// Re-open the speaker at another sample rate without changing ownership.
esp_err_t panel_audio_set_output_rate(uint32_t rate);

// Sample rate the speaker is currently opened with.
uint32_t panel_audio_get_output_rate(void);

// True while the radio (or any other exclusive user) owns the speaker.
bool panel_audio_output_is_exclusive(void);
