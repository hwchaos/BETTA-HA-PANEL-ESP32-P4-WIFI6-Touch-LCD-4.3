/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ipa.h"
#include "esp_video_isp_ioctl.h"
#include "esp_video_isp_manual.h"
#include "esp_video_isp_manual_internal.h"

/* ISP hardware limits, see hal/isp_ll.h and driver/isp_bf.h. */
#define SHARPEN_COEFF_MAX       (7.9f)      /* h_coeff/m_coeff are stored as 5.5 fixed point */
#define DENOISE_LEVEL_MIN       (2)
#define DENOISE_LEVEL_MAX       (20)

/* Maximum gamma curve shift of the shadow/highlight control. */
#define TONE_SHIFT_MAX          (0.30f)

/* Blocks whose IPA result is cached, so that a manual override keeps working on
 * frames where the IPA did not touch the block (start-up, block disabled in the
 * IPA configuration). */
#define MANUAL_CACHED_FLAGS     (IPA_METADATA_FLAGS_BR | IPA_METADATA_FLAGS_CN |   \
                                 IPA_METADATA_FLAGS_ST | IPA_METADATA_FLAGS_HUE |  \
                                 IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG |   \
                                 IPA_METADATA_FLAGS_SH | IPA_METADATA_FLAGS_BF |   \
                                 IPA_METADATA_FLAGS_GAMMA)

static const char *TAG = "isp_manual";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_video_isp_manual_t s_config = {
    .contrast = 128,
    .saturation = 128,
    .wb_red_gain = 1.0f,
    .wb_blue_gain = 1.0f,
    .sharpen_gain = 1.0f,
    .denoise_scale = 1.0f,
};

/* Last IPA values per block, only touched by the ISP task. */
static esp_ipa_metadata_t s_base;

static inline float clamp_float(float value, float min, float max)
{
    if (value < min) {
        return min;
    }

    return (value > max) ? max : value;
}

static inline int32_t clamp_int(int32_t value, int32_t min, int32_t max)
{
    if (value < min) {
        return min;
    }

    return (value > max) ? max : value;
}

static void esp_video_isp_manual_cache(const esp_ipa_metadata_t *metadata)
{
    uint32_t fresh = metadata->flags & MANUAL_CACHED_FLAGS;

    if (fresh & IPA_METADATA_FLAGS_BR) {
        s_base.brightness = metadata->brightness;
    }
    if (fresh & IPA_METADATA_FLAGS_CN) {
        s_base.contrast = metadata->contrast;
    }
    if (fresh & IPA_METADATA_FLAGS_ST) {
        s_base.saturation = metadata->saturation;
    }
    if (fresh & IPA_METADATA_FLAGS_HUE) {
        s_base.hue = metadata->hue;
    }
    if (fresh & IPA_METADATA_FLAGS_RG) {
        s_base.red_gain = metadata->red_gain;
    }
    if (fresh & IPA_METADATA_FLAGS_BG) {
        s_base.blue_gain = metadata->blue_gain;
    }
    if (fresh & IPA_METADATA_FLAGS_SH) {
        s_base.sharpen = metadata->sharpen;
    }
    if (fresh & IPA_METADATA_FLAGS_BF) {
        s_base.bf = metadata->bf;
    }
    if (fresh & IPA_METADATA_FLAGS_GAMMA) {
        s_base.gamma = metadata->gamma;
    }

    s_base.flags |= fresh;
}

void esp_video_isp_manual_apply(esp_ipa_metadata_t *metadata)
{
    esp_video_isp_manual_t config;

    esp_video_isp_manual_cache(metadata);

    portENTER_CRITICAL(&s_lock);
    config = s_config;
    portEXIT_CRITICAL(&s_lock);

    if ((config.blocks & ESP_VIDEO_ISP_MANUAL_BRIGHTNESS) != 0) {
        /* The ISP brightness is a signed offset, keep the sign when storing it. */
        metadata->brightness = (uint32_t)(int32_t)config.brightness;
        metadata->flags |= IPA_METADATA_FLAGS_BR;
    }

    if ((config.blocks & ESP_VIDEO_ISP_MANUAL_CONTRAST) != 0) {
        metadata->contrast = config.contrast;
        metadata->flags |= IPA_METADATA_FLAGS_CN;
    }

    if ((config.blocks & ESP_VIDEO_ISP_MANUAL_SATURATION) != 0) {
        metadata->saturation = config.saturation;
        metadata->flags |= IPA_METADATA_FLAGS_ST;
    }

    if ((config.blocks & ESP_VIDEO_ISP_MANUAL_HUE) != 0) {
        metadata->hue = config.hue;
        metadata->flags |= IPA_METADATA_FLAGS_HUE;
    }

    if ((config.blocks & ESP_VIDEO_ISP_MANUAL_WB) != 0) {
        float red_gain = (s_base.flags & IPA_METADATA_FLAGS_RG) ? s_base.red_gain : 1.0f;
        float blue_gain = (s_base.flags & IPA_METADATA_FLAGS_BG) ? s_base.blue_gain : 1.0f;

        metadata->red_gain = red_gain * config.wb_red_gain;
        metadata->blue_gain = blue_gain * config.wb_blue_gain;
        metadata->flags |= IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG;
    }

    if (((config.blocks & ESP_VIDEO_ISP_MANUAL_SHARPEN) != 0) && (s_base.flags & IPA_METADATA_FLAGS_SH)) {
        metadata->sharpen = s_base.sharpen;
        metadata->sharpen.h_coeff = clamp_float(s_base.sharpen.h_coeff * config.sharpen_gain, 0.0f, SHARPEN_COEFF_MAX);
        metadata->sharpen.m_coeff = clamp_float(s_base.sharpen.m_coeff * config.sharpen_gain, 0.0f, SHARPEN_COEFF_MAX);
        metadata->flags |= IPA_METADATA_FLAGS_SH;
    }

    if (((config.blocks & ESP_VIDEO_ISP_MANUAL_DENOISE) != 0) && (s_base.flags & IPA_METADATA_FLAGS_BF)) {
        int32_t level = (int32_t)lroundf((float)s_base.bf.level * config.denoise_scale);

        metadata->bf = s_base.bf;
        metadata->bf.level = (uint8_t)clamp_int(level, DENOISE_LEVEL_MIN, DENOISE_LEVEL_MAX);
        metadata->flags |= IPA_METADATA_FLAGS_BF;
    }

    if (((config.blocks & ESP_VIDEO_ISP_MANUAL_TONE) != 0) && (s_base.flags & IPA_METADATA_FLAGS_GAMMA)) {
        metadata->gamma = s_base.gamma;

        for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
            float x = (float)s_base.gamma.x[i] / 255.0f;
            float y = (float)s_base.gamma.y[i] / 255.0f;
            /* Cubic weights: the shadow control fades out towards white and the
             * highlight control fades out towards black. */
            float shadow_weight = (1.0f - x) * (1.0f - x) * (1.0f - x);
            float highlight_weight = x * x * x;

            y += config.tone_shadows * shadow_weight * TONE_SHIFT_MAX;
            y += config.tone_highlights * highlight_weight * TONE_SHIFT_MAX;
            metadata->gamma.y[i] = (uint8_t)clamp_int(lroundf(y * 255.0f), 0, 255);
        }

        metadata->flags |= IPA_METADATA_FLAGS_GAMMA;
    }
}

esp_err_t esp_video_isp_manual_set(const esp_video_isp_manual_t *config)
{
    esp_video_isp_manual_t value;

    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    value = *config;
    value.blocks &= ESP_VIDEO_ISP_MANUAL_ALL;
    value.brightness = clamp_int(value.brightness, ESP_VIDEO_ISP_MANUAL_BRIGHTNESS_MIN, ESP_VIDEO_ISP_MANUAL_BRIGHTNESS_MAX);
    value.contrast = (uint32_t)clamp_int((int32_t)value.contrast, 0, ESP_VIDEO_ISP_MANUAL_CONTRAST_MAX);
    value.saturation = (uint32_t)clamp_int((int32_t)value.saturation, 0, ESP_VIDEO_ISP_MANUAL_SATURATION_MAX);
    value.hue = (uint32_t)clamp_int((int32_t)value.hue, 0, ESP_VIDEO_ISP_MANUAL_HUE_MAX);
    value.wb_red_gain = clamp_float(value.wb_red_gain, ESP_VIDEO_ISP_MANUAL_WB_GAIN_MIN, ESP_VIDEO_ISP_MANUAL_WB_GAIN_MAX);
    value.wb_blue_gain = clamp_float(value.wb_blue_gain, ESP_VIDEO_ISP_MANUAL_WB_GAIN_MIN, ESP_VIDEO_ISP_MANUAL_WB_GAIN_MAX);
    value.sharpen_gain = clamp_float(value.sharpen_gain, ESP_VIDEO_ISP_MANUAL_SHARPEN_MIN, ESP_VIDEO_ISP_MANUAL_SHARPEN_MAX);
    value.denoise_scale = clamp_float(value.denoise_scale, ESP_VIDEO_ISP_MANUAL_DENOISE_MIN, ESP_VIDEO_ISP_MANUAL_DENOISE_MAX);
    value.tone_shadows = clamp_float(value.tone_shadows, ESP_VIDEO_ISP_MANUAL_TONE_MIN, ESP_VIDEO_ISP_MANUAL_TONE_MAX);
    value.tone_highlights = clamp_float(value.tone_highlights, ESP_VIDEO_ISP_MANUAL_TONE_MIN, ESP_VIDEO_ISP_MANUAL_TONE_MAX);

    portENTER_CRITICAL(&s_lock);
    s_config = value;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGD(TAG, "manual calibration blocks=0x%02x brightness=%d contrast=%u saturation=%u",
             (unsigned)value.blocks, (int)value.brightness, (unsigned)value.contrast, (unsigned)value.saturation);

    return ESP_OK;
}

esp_err_t esp_video_isp_manual_get(esp_video_isp_manual_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    portENTER_CRITICAL(&s_lock);
    *config = s_config;
    portEXIT_CRITICAL(&s_lock);

    return ESP_OK;
}

void esp_video_isp_manual_reset(void)
{
    esp_video_isp_manual_t value = ESP_VIDEO_ISP_MANUAL_NEUTRAL();

    portENTER_CRITICAL(&s_lock);
    s_config = value;
    portEXIT_CRITICAL(&s_lock);
}
