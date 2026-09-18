/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Manual ISP image calibration.
 *
 * The IPA pipeline recalculates the ISP blocks on every frame, so V4L2 control
 * writes issued by an application are overwritten again after at most one frame.
 * This module holds a manual override set which is merged into the IPA meta data
 * right before it is written to the ISP hardware, so manual values stick.
 *
 * Each parameter group is opt-in through the "blocks" bit mask. While a block is
 * not selected it is controlled by the IPA exactly as before, which keeps the
 * default behaviour (blocks == 0) identical to the stock component.
 */

#define ESP_VIDEO_ISP_MANUAL_BRIGHTNESS     (1U << 0)   /*!< Absolute brightness offset */
#define ESP_VIDEO_ISP_MANUAL_CONTRAST       (1U << 1)   /*!< Absolute contrast */
#define ESP_VIDEO_ISP_MANUAL_SATURATION     (1U << 2)   /*!< Absolute saturation */
#define ESP_VIDEO_ISP_MANUAL_HUE            (1U << 3)   /*!< Absolute hue */
#define ESP_VIDEO_ISP_MANUAL_WB             (1U << 4)   /*!< AWB gain multiplier */
#define ESP_VIDEO_ISP_MANUAL_SHARPEN        (1U << 5)   /*!< Sharpen coefficient multiplier */
#define ESP_VIDEO_ISP_MANUAL_DENOISE        (1U << 6)   /*!< Bayer denoise level multiplier */
#define ESP_VIDEO_ISP_MANUAL_TONE           (1U << 7)   /*!< Shadow/highlight tone curve */
#define ESP_VIDEO_ISP_MANUAL_ALL            (0xFFU)     /*!< All parameter groups */

/* Ranges accepted by "esp_video_isp_manual_set()"; values outside are clamped. */
#define ESP_VIDEO_ISP_MANUAL_BRIGHTNESS_MIN (-128)
#define ESP_VIDEO_ISP_MANUAL_BRIGHTNESS_MAX (127)
#define ESP_VIDEO_ISP_MANUAL_CONTRAST_MAX   (255)
#define ESP_VIDEO_ISP_MANUAL_SATURATION_MAX (255)
#define ESP_VIDEO_ISP_MANUAL_HUE_MAX        (360)
#define ESP_VIDEO_ISP_MANUAL_WB_GAIN_MIN    (0.25f)
#define ESP_VIDEO_ISP_MANUAL_WB_GAIN_MAX    (4.0f)
#define ESP_VIDEO_ISP_MANUAL_SHARPEN_MIN    (0.25f)
#define ESP_VIDEO_ISP_MANUAL_SHARPEN_MAX    (3.0f)
#define ESP_VIDEO_ISP_MANUAL_DENOISE_MIN    (0.25f)
#define ESP_VIDEO_ISP_MANUAL_DENOISE_MAX    (2.0f)
#define ESP_VIDEO_ISP_MANUAL_TONE_MIN       (-1.0f)
#define ESP_VIDEO_ISP_MANUAL_TONE_MAX       (1.0f)

/**
 * @brief Manual calibration parameters.
 *
 * Values equal to the neutral ones (see ESP_VIDEO_ISP_MANUAL_NEUTRAL) leave the
 * image untouched, so a block can be enabled without changing anything.
 */
typedef struct {
    uint32_t blocks;            /*!< Bit mask of ESP_VIDEO_ISP_MANUAL_* */
    int32_t brightness;         /*!< [-128, 127], 0 is neutral */
    uint32_t contrast;          /*!< [0, 255], 128 is neutral */
    uint32_t saturation;        /*!< [0, 255], 128 is neutral */
    uint32_t hue;               /*!< [0, 360], 0 is neutral */
    float wb_red_gain;          /*!< [0.25, 4.0] multiplier on the AWB red gain, 1.0 is neutral */
    float wb_blue_gain;         /*!< [0.25, 4.0] multiplier on the AWB blue gain, 1.0 is neutral */
    float sharpen_gain;         /*!< [0.25, 3.0] multiplier on the IPA sharpen coefficients, 1.0 is neutral */
    float denoise_scale;        /*!< [0.25, 2.0] multiplier on the IPA denoise level, 1.0 is neutral */
    float tone_shadows;         /*!< [-1.0, 1.0] lift (positive) or crush (negative) dark tones, 0.0 is neutral */
    float tone_highlights;      /*!< [-1.0, 1.0] boost (positive) or recover (negative) bright tones, 0.0 is neutral */
} esp_video_isp_manual_t;

/**
 * @brief Neutral calibration: no manual override at all.
 */
#define ESP_VIDEO_ISP_MANUAL_NEUTRAL()                  \
    {                                                   \
        .blocks = 0,                                    \
        .brightness = 0,                                \
        .contrast = 128,                                \
        .saturation = 128,                              \
        .hue = 0,                                       \
        .wb_red_gain = 1.0f,                            \
        .wb_blue_gain = 1.0f,                           \
        .sharpen_gain = 1.0f,                           \
        .denoise_scale = 1.0f,                          \
        .tone_shadows = 0.0f,                           \
        .tone_highlights = 0.0f,                        \
    }

/**
 * @brief Set the manual calibration parameters.
 *
 * Can be called at any time from any task, also while the camera is streaming.
 * The new parameters are picked up by the ISP task within one frame, if the
 * camera is running.
 *
 * @param config  Manual calibration parameters, values are clamped to the ranges
 *                documented in esp_video_isp_manual_t
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config is NULL
 */
esp_err_t esp_video_isp_manual_set(const esp_video_isp_manual_t *config);

/**
 * @brief Get the manual calibration parameters currently in use.
 *
 * @param config  Output manual calibration parameters
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config is NULL
 */
esp_err_t esp_video_isp_manual_get(esp_video_isp_manual_t *config);

/**
 * @brief Drop all manual overrides and give every block back to the IPA.
 */
void esp_video_isp_manual_reset(void);

#ifdef __cplusplus
}
#endif
