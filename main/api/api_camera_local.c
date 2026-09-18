/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * HTTP endpoints for the built-in MIPI-CSI camera (Guition JC8012P4A1C).
 */
#include "api/api_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "camera/local_camera.h"
#include "esp_video_isp_manual.h"
#include "util/log_tags.h"

#define CAMERA_STREAM_FRAME_MS 500
#define CAMERA_STREAM_STACK_WORDS 4096
#define CAMERA_STREAM_PRIO 5

static bool s_stream_enabled = false;
static TaskHandle_t s_stream_task = NULL;

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

esp_err_t api_camera_local_snapshot_get_handler(httpd_req_t *req)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;

    esp_err_t err = local_camera_snapshot_jpeg(&jpeg, &jpeg_len);
    if (err != ESP_OK || jpeg == NULL || jpeg_len == 0) {
        if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "{\"error\":\"camera_not_ready\"}");
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"snapshot_failed\"}");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t send_err = httpd_resp_send(req, (const char *)jpeg, (ssize_t)jpeg_len);
    free(jpeg);
    return send_err;
}

esp_err_t api_camera_local_status_get_handler(httpd_req_t *req)
{
    esp_video_isp_manual_t calib = ESP_VIDEO_ISP_MANUAL_NEUTRAL();
    (void)esp_video_isp_manual_get(&calib);

    /* The calibration values are reported as percentages of the neutral value,
     * which is what the settings UI and the web UI use as well. */
    char json[512];
    snprintf(json, sizeof(json),
             "{\"running\":%s,\"width\":%d,\"height\":%d,\"resolution\":%d,"
             "\"motion_wake\":%s,\"motion_threshold\":%u,\"jpeg_quality\":%u,"
             "\"hflip\":%s,\"vflip\":%s,"
             "\"image\":{\"blocks\":%u,\"brightness\":%d,\"contrast\":%u,\"saturation\":%u,"
             "\"hue\":%u,\"wb_red\":%d,\"wb_blue\":%d,\"sharpen\":%d,\"denoise\":%d,"
             "\"tone_shadows\":%d,\"tone_highlights\":%d}}",
             local_camera_is_running() ? "true" : "false",
             local_camera_width(),
             local_camera_height(),
             local_camera_get_resolution(),
             local_camera_get_motion_wake() ? "true" : "false",
             (unsigned)local_camera_get_motion_threshold(),
             (unsigned)local_camera_get_jpeg_quality(),
             local_camera_get_hflip() ? "true" : "false",
             local_camera_get_vflip() ? "true" : "false",
             (unsigned)calib.blocks,
             (int)calib.brightness,
             (unsigned)calib.contrast,
             (unsigned)calib.saturation,
             (unsigned)calib.hue,
             (int)(calib.wb_red_gain * 100.0f + 0.5f),
             (int)(calib.wb_blue_gain * 100.0f + 0.5f),
             (int)(calib.sharpen_gain * 100.0f + 0.5f),
             (int)(calib.denoise_scale * 100.0f + 0.5f),
             (int)(calib.tone_shadows * 100.0f + (calib.tone_shadows < 0 ? -0.5f : 0.5f)),
             (int)(calib.tone_highlights * 100.0f + (calib.tone_highlights < 0 ? -0.5f : 0.5f)));

    set_json_headers(req);
    return httpd_resp_sendstr(req, json);
}

esp_err_t api_camera_local_motion_get_handler(httpd_req_t *req)
{
    local_camera_motion_status_t status;
    local_camera_motion_config_t config;
    local_camera_get_motion_status(&status);
    local_camera_get_motion_config(&config);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddNumberToObject(root, "threshold", (double)status.threshold);
    cJSON_AddNumberToObject(root, "level", (double)status.last_level);
    cJSON_AddNumberToObject(root, "changed_pct", (double)status.last_changed_pct);
    cJSON_AddBoolToObject(root, "ignored_lighting", status.last_ignored_lighting);
    cJSON_AddBoolToObject(root, "active", status.active);
    cJSON_AddNumberToObject(root, "trigger_count", (double)status.trigger_count);
    cJSON_AddNumberToObject(root, "last_trigger_ms", (double)status.last_trigger_ms);

    cJSON *zones = cJSON_CreateArray();
    if (zones == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    for (uint8_t z = 0; z < config.zone_count && z < LOCAL_CAMERA_MOTION_MAX_ZONES; z++) {
        cJSON *zone = cJSON_CreateObject();
        if (zone == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(zone, "x", (double)config.zones[z].x);
        cJSON_AddNumberToObject(zone, "y", (double)config.zones[z].y);
        cJSON_AddNumberToObject(zone, "w", (double)config.zones[z].w);
        cJSON_AddNumberToObject(zone, "h", (double)config.zones[z].h);
        cJSON_AddNumberToObject(zone, "level", (double)status.zone_level[z]);
        cJSON_AddNumberToObject(zone, "changed_pct", (double)status.zone_changed_pct[z]);
        cJSON_AddItemToArray(zones, zone);
    }
    cJSON_AddItemToObject(root, "zones", zones);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    free(payload);
    return err;
}

void api_camera_local_set_stream_enabled(bool enabled)
{
    s_stream_enabled = enabled;
}

bool api_camera_local_get_stream_enabled(void)
{
    return s_stream_enabled;
}

static void camera_stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    char header[128];

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");

    while (s_stream_enabled && local_camera_is_running()) {
        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;

        esp_err_t err = local_camera_snapshot_jpeg(&jpeg, &jpeg_len);
        if (err != ESP_OK || jpeg == NULL || jpeg_len == 0) {
            free(jpeg);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_STREAM_FRAME_MS));
            continue;
        }

        int hdr_len = snprintf(header, sizeof(header),
                               "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned)jpeg_len);
        if (hdr_len <= 0 || (size_t)hdr_len >= sizeof(header)) {
            free(jpeg);
            break;
        }

        esp_err_t send_err = httpd_resp_send_chunk(req, header, (ssize_t)hdr_len);
        if (send_err == ESP_OK) {
            send_err = httpd_resp_send_chunk(req, (const char *)jpeg, (ssize_t)jpeg_len);
        }
        if (send_err == ESP_OK) {
            send_err = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        free(jpeg);
        if (send_err != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(CAMERA_STREAM_FRAME_MS));
    }

    httpd_req_async_handler_complete(req);
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t api_camera_local_stream_get_handler(httpd_req_t *req)
{
    if (!s_stream_enabled) {
        set_json_headers(req);
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"error\":\"stream_disabled\"}");
    }
    if (s_stream_task != NULL) {
        set_json_headers(req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"stream_busy\"}");
    }
    if (!local_camera_is_running()) {
        set_json_headers(req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"camera_not_ready\"}");
    }

    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK || copy == NULL) {
        set_json_headers(req);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"stream_start_failed\"}");
    }

    if (xTaskCreate(camera_stream_task, "cam_stream", CAMERA_STREAM_STACK_WORDS,
                    copy, CAMERA_STREAM_PRIO, &s_stream_task) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        s_stream_task = NULL;
    }

    /* The socket is owned by the async task from this point on. */
    return ESP_OK;
}
