/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Built-in OV5647 MIPI-CSI camera of the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
 * panel (sensor on the shared BSP I2C/SCCB bus).
 *
 * The sensor driver (esp_cam_sensor 2.1.0) is vendored under components/ and
 * pulled in by the ESP Video framework (esp_video ~2.0).  This module owns the
 * V4L2 capture pipeline (/dev/video0): continuous RGB565 capture into PSRAM
 * USERPTR buffers, a mutex-guarded "latest frame" cache, a lightweight
 * frame-difference motion detector for screen wake, and on-demand hardware
 * JPEG encoding for the web snapshot endpoint.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*local_camera_motion_cb_t)(void *user_data);

/* Motion detector tuning (point 1 of the motion-detection upgrade). */
#define LOCAL_CAMERA_MOTION_MAX_ZONES 4

typedef struct {
    uint8_t x; /* left edge, percent of frame width 0..100 */
    uint8_t y; /* top edge, percent of frame height 0..100 */
    uint8_t w; /* width, percent of frame width 0..100 */
    uint8_t h; /* height, percent of frame height 0..100 */
} local_camera_motion_zone_t;

typedef struct {
    uint8_t threshold;       /* 1..64, mean-abs-diff sensitivity */
    uint8_t min_area_pct;    /* 0..100 % of active cells that must change, 0 = off */
    uint16_t min_duration_ms; /* 0..1000 ms the change must persist, 0 = off */
    uint16_t cooldown_ms;    /* 0..30000 ms between two callbacks */
    uint16_t start_delay_ms; /* 0..10000 ms grace period after STREAMON */
    bool ignore_lighting;    /* suppress global brightness shifts */
    uint8_t zone_count;      /* 0..4; 0 = whole frame (legacy) */
    local_camera_motion_zone_t zones[LOCAL_CAMERA_MOTION_MAX_ZONES];
} local_camera_motion_config_t;

typedef struct {
    uint8_t threshold;
    uint8_t last_level;        /* last mean-abs-diff (0..255) */
    uint8_t last_changed_pct;  /* last changed-cell percentage (0..100) */
    bool last_ignored_lighting; /* last frame was rejected as a lighting change */
    bool active;               /* currently above threshold (debouncing) */
    uint32_t trigger_count;    /* motion callbacks fired since boot */
    int64_t last_trigger_ms;   /* esp_timer ms of the last callback */
    uint8_t zone_count;        /* 0 = whole-frame virtual zone */
    uint8_t zone_level[LOCAL_CAMERA_MOTION_MAX_ZONES];       /* per-zone mean diff */
    uint8_t zone_changed_pct[LOCAL_CAMERA_MOTION_MAX_ZONES]; /* per-zone changed % */
} local_camera_motion_status_t;

/* Start the camera pipeline.  Reuses the touch/codec I2C bus (I2C_NUM_1,
 * SCL=8/SDA=7) for the sensor SCCB interface.  Safe to call repeatedly. */
esp_err_t local_camera_start(void);

/* Stop the stream task and release the video device.  Keep allocated frame
 * buffers so a later start() is cheap; use local_camera_deinit() to free. */
esp_err_t local_camera_stop(void);

/* Full teardown (stop + free buffers/JPEG engine). */
void local_camera_deinit(void);

bool local_camera_is_running(void);

/* Enable/disable the motion-wake detector (display_note_activity on motion). */
void local_camera_set_motion_wake(bool enabled);

/* Runtime motion sensitivity: mean-absolute-difference threshold over the
 * 32x32 luma grid.  Higher values = less sensitive.  Range 1..64. */
void local_camera_set_motion_threshold(uint8_t threshold);

/* Apply the full motion-detector configuration (zones, debounce, cooldown,
 * start-delay grace period and the global-lighting filter) atomically.  Safe
 * to call before the pipeline is started: the values persist in the component
 * state and take effect as soon as frames flow. */
void local_camera_set_motion_config(const local_camera_motion_config_t *config);
void local_camera_get_motion_config(local_camera_motion_config_t *config);

/* Read live motion-detector diagnostics (per-zone levels, counters, state). */
void local_camera_get_motion_status(local_camera_motion_status_t *status);

/* Runtime JPEG quality used by local_camera_snapshot_jpeg().  Range 10..95. */
void local_camera_set_jpeg_quality(uint8_t quality);

/* Runtime mirroring.  Applied immediately if the pipeline is running. */
void local_camera_set_flip(bool hflip, bool vflip);

/* Resolution mode: 0 = native size of the sensor mode selected in Kconfig
 * (1280x960 for the default MIPI RAW10 binning mode), 1 = half of it
 * (640x480), produced by a 2x box-average filter.
 * Changing the mode while the pipeline is running requires a restart so the
 * capture-side frame cache and JPEG buffers are reallocated for the new size;
 * local_camera_apply_settings() handles that automatically. */
void local_camera_set_resolution(int resolution);
int local_camera_get_resolution(void);

/* Apply every camera parameter in one call (used by the web settings API and
 * the on-panel settings page).  Starts/stops the pipeline when "enabled"
 * changes, restarts it when "resolution" changes while running, and applies
 * the remaining parameters in place.  Returns ESP_OK on success. */
esp_err_t local_camera_apply_settings(bool enabled, bool motion_wake, uint8_t motion_threshold,
                                      uint8_t jpeg_quality, bool hflip, bool vflip,
                                      int resolution);

/* Keep the internal frame cache warm for hold_ms from now.  The capture loop
 * only copies frames out of the CSI buffers while a consumer asked for them,
 * which keeps the camera off the PSRAM bus (and away from the DSI scan-out)
 * the rest of the time.  Call this before reading local_camera_snapshot_jpeg()
 * or local_camera_copy_scaled_rgb565() when a continuous stream is wanted;
 * both arm the cache themselves, so single shots need no extra call. */
void local_camera_demand_frames(uint32_t hold_ms);

/* Copy the latest captured frame, scaled to dst_w x dst_h (nearest neighbour),
 * into the caller-provided RGB565 buffer.  dst must hold dst_w*dst_h*2 bytes.
 * Returns ESP_ERR_INVALID_STATE when the camera is off or no frame is ready. */
esp_err_t local_camera_copy_scaled_rgb565(uint8_t *dst, int dst_w, int dst_h);

/* Encode the latest captured frame to JPEG and return a freshly allocated
 * buffer.  The caller owns *out_buf and must free() it. */
esp_err_t local_camera_snapshot_jpeg(uint8_t **out_buf, size_t *out_len);

/* Register a callback fired when motion is detected. */
esp_err_t local_camera_register_motion_cb(local_camera_motion_cb_t cb, void *user_data);

bool local_camera_get_motion_wake(void);
uint8_t local_camera_get_motion_threshold(void);
uint8_t local_camera_get_jpeg_quality(void);
bool local_camera_get_hflip(void);
bool local_camera_get_vflip(void);

int local_camera_width(void);
int local_camera_height(void);

#ifdef __cplusplus
}
#endif
