/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#pragma once

#include "esp_ipa_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Merge the manual calibration into the IPA meta data of the current frame.
 *
 * Must be called by the ISP task between "esp_ipa_pipeline_process()" and
 * "config_isp_and_camera()".
 *
 * @param metadata  IPA meta data of the current frame
 */
void esp_video_isp_manual_apply(esp_ipa_metadata_t *metadata);

#ifdef __cplusplus
}
#endif
