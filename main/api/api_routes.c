/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"
#include "api/http_guard.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "radio/panel_radio.h"
#include "ui/ui_radio_stations.h"

/* TEMPORARY: lets a PC drive the on-panel radio engine (index from the
 * built-in station table) so the audio path can be exercised without touching
 * the screen. Remove together with its route in api_routes_register(). */
static esp_err_t api_radio_get_handler(httpd_req_t *req)
{
    char query[96] = {0};
    char cmd[16] = {0};
    char index_txt[8] = {0};
    char value_txt[8] = {0};

    if (httpd_req_get_url_query_len(req) > 0 && httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "cmd", cmd, sizeof(cmd));
        (void)httpd_query_key_value(query, "st", index_txt, sizeof(index_txt));
        (void)httpd_query_key_value(query, "v", value_txt, sizeof(value_txt));
    }

    const ui_radio_station_t *station = NULL;
    if (strcmp(cmd, "stop") == 0) {
        panel_radio_stop();
    } else if (strcmp(cmd, "dec") == 0) {
        /* A/B switch for the decoder's rejection log: the flood costs the
         * decode task ~30% of its time (see pr_task_start). */
        const bool on = strcmp(value_txt, "1") == 0;
        esp_log_level_set("ESP_AAC_DEC", on ? ESP_LOG_ERROR : ESP_LOG_NONE);
        esp_log_level_set("ESP_MP3_DEC", on ? ESP_LOG_ERROR : ESP_LOG_NONE);
    } else if (strcmp(cmd, "state") != 0) {
        const long index = strtol(index_txt, NULL, 10);
        if (index >= 0) {
            station = ui_radio_stations_get((size_t)index);
        }
        if (station == NULL) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"st index out of range\"}");
        }
        (void)panel_radio_play(station->url, station->name);
    }

    char title[PANEL_RADIO_TITLE_MAX] = {0};
    char error[PANEL_RADIO_TEXT_MAX] = {0};
    panel_radio_get_title(title, sizeof(title));
    panel_radio_get_error(error, sizeof(error));

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    if (station != NULL) {
        cJSON_AddStringToObject(root, "playing", station->name);
    }
    cJSON_AddNumberToObject(root, "state", (double)panel_radio_get_state());
    cJSON_AddStringToObject(root, "title", title);
    cJSON_AddStringToObject(root, "error", error);
    cJSON_AddNumberToObject(root, "rate", (double)panel_radio_get_rate());
    cJSON_AddNumberToObject(root, "heap", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* TEMPORARY (see panel_radio_stats_t): the ring is turned over by the
     * decoder flood, so the counters are read here instead of the log. */
    panel_radio_stats_t stats = {0};
    panel_radio_get_stats(&stats);
    cJSON_AddNumberToObject(root, "samples", (double)stats.samples);
    cJSON_AddNumberToObject(root, "calls", (double)stats.calls);
    cJSON_AddNumberToObject(root, "ok", (double)stats.ok);
    cJSON_AddNumberToObject(root, "decerr", (double)stats.decerr);
    cJSON_AddNumberToObject(root, "resync", (double)stats.resync);
    cJSON_AddNumberToObject(root, "bytes_in", (double)stats.bytes_in);
    cJSON_AddNumberToObject(root, "window_s", (double)stats.window_s);
    cJSON_AddNumberToObject(root, "samples_s", (double)stats.samples_s);
    cJSON_AddNumberToObject(root, "ok_s", (double)stats.ok_s);
    cJSON_AddNumberToObject(root, "calls_s", (double)stats.calls_s);
    cJSON_AddNumberToObject(root, "err_s", (double)stats.err_s);
    cJSON_AddNumberToObject(root, "resync_s", (double)stats.resync_s);
    cJSON_AddNumberToObject(root, "pcm_per_ok", (double)stats.pcm_per_ok);
    cJSON_AddNumberToObject(root, "in_bps", (double)stats.in_bps);
    cJSON_AddNumberToObject(root, "ring_kb", (double)stats.ring_kb);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static esp_err_t guarded_api_radio_debug_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_radio_get_handler);
}

static esp_err_t guarded_api_layout_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_layout_get_handler);
}

static esp_err_t guarded_api_layout_put(httpd_req_t *req)
{
    return http_guard_handle(req, api_layout_put_handler);
}

static esp_err_t guarded_api_entities_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_entities_get_handler);
}

static esp_err_t guarded_api_light_entities_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_light_entities_get_handler);
}

static esp_err_t guarded_api_light_entities_delete(httpd_req_t *req)
{
    return http_guard_handle(req, api_light_entities_delete_handler);
}

static esp_err_t guarded_api_ha_energy_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_ha_energy_get_handler);
}

static esp_err_t guarded_api_state_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_state_get_handler);
}

static esp_err_t guarded_api_settings_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_settings_get_handler);
}

static esp_err_t guarded_api_settings_put(httpd_req_t *req)
{
    return http_guard_handle(req, api_settings_put_handler);
}

static esp_err_t guarded_api_sd_status_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_sd_status_get_handler);
}

static esp_err_t guarded_api_sd_status_put(httpd_req_t *req)
{
    return http_guard_handle(req, api_sd_status_put_handler);
}

static esp_err_t guarded_api_sd_format_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_sd_format_post_handler);
}

static esp_err_t guarded_api_sd_files_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_sd_files_get_handler);
}

static esp_err_t guarded_api_sd_file_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_sd_file_get_handler);
}

static esp_err_t guarded_api_sd_file_delete(httpd_req_t *req)
{
    return http_guard_handle(req, api_sd_file_delete_handler);
}

static esp_err_t guarded_api_sd_logs_export_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_sd_logs_export_post_handler);
}

static esp_err_t guarded_api_display_activity_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_display_activity_post_handler);
}

static esp_err_t guarded_api_display_wallpaper_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_display_wallpaper_post_handler);
}

static esp_err_t guarded_api_display_wallpaper_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_display_wallpaper_get_handler);
}

static esp_err_t guarded_api_display_wallpaper_delete(httpd_req_t *req)
{
    return http_guard_handle(req, api_display_wallpaper_delete_handler);
}

static esp_err_t guarded_api_i18n_languages_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_i18n_languages_get_handler);
}

static esp_err_t guarded_api_i18n_effective_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_i18n_effective_get_handler);
}

static esp_err_t guarded_api_i18n_custom_put(httpd_req_t *req)
{
    return http_guard_handle(req, api_i18n_custom_put_handler);
}

static esp_err_t guarded_api_wifi_scan_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_wifi_scan_get_handler);
}

static esp_err_t guarded_api_version_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_version_get_handler);
}

static esp_err_t guarded_api_screenshot_bmp_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_screenshot_bmp_get_handler);
}

static esp_err_t guarded_api_ota_status_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_ota_status_get_handler);
}

static esp_err_t guarded_api_ota_url_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_ota_url_post_handler);
}

static esp_err_t guarded_api_ota_upload_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_ota_upload_post_handler);
}

static esp_err_t guarded_api_ha_diagnostics_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_ha_diagnostics_get_handler);
}

static esp_err_t guarded_api_cameras_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_cameras_get_handler);
}

static esp_err_t guarded_api_cameras_put(httpd_req_t *req)
{
    return http_guard_handle(req, api_cameras_put_handler);
}

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
static esp_err_t guarded_api_camera_local_snapshot_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_camera_local_snapshot_get_handler);
}

static esp_err_t guarded_api_camera_local_status_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_camera_local_status_get_handler);
}

static esp_err_t guarded_api_camera_local_motion_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_camera_local_motion_get_handler);
}

static esp_err_t guarded_api_camera_local_stream_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_camera_local_stream_get_handler);
}
#endif

static esp_err_t guarded_api_diagnostics_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_diagnostics_get_handler);
}

static esp_err_t guarded_api_status_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_status_get_handler);
}

static esp_err_t guarded_api_logs_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_logs_get_handler);
}

static esp_err_t guarded_api_logs_delete(httpd_req_t *req)
{
    return http_guard_handle(req, api_logs_delete_handler);
}

static esp_err_t guarded_api_crash_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_crash_get_handler);
}

static esp_err_t guarded_api_crash_raw_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_crash_raw_get_handler);
}

static esp_err_t guarded_api_crash_erase_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_crash_erase_post_handler);
}

static esp_err_t guarded_api_backup_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_backup_get_handler);
}

static esp_err_t guarded_api_backup_restore_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_backup_restore_post_handler);
}
static esp_err_t guarded_api_themes_list_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_themes_list_get_handler);
}
static esp_err_t guarded_api_themes_active_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_themes_active_get_handler);
}
static esp_err_t guarded_api_themes_active_put(httpd_req_t *req)
{
    return http_guard_handle(req, api_themes_active_put_handler);
}
static esp_err_t guarded_api_themes_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_themes_get_handler);
}
static esp_err_t guarded_api_themes_custom_put(httpd_req_t *req)
{
    return http_guard_handle(req, api_themes_custom_put_handler);
}
static esp_err_t guarded_api_themes_custom_delete(httpd_req_t *req)
{
    return http_guard_handle(req, api_themes_custom_delete_handler);
}
static esp_err_t guarded_api_pages_get(httpd_req_t *req)
{
    return http_guard_handle(req, api_pages_get_handler);
}
static esp_err_t guarded_api_pages_activate_post(httpd_req_t *req)
{
    return http_guard_handle(req, api_pages_activate_post_handler);
}

esp_err_t api_routes_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_uri_t get_layout = {
        .uri = "/api/layout",
        .method = HTTP_GET,
        .handler = guarded_api_layout_get,
        .user_ctx = NULL,
    };
    httpd_uri_t put_layout = {
        .uri = "/api/layout",
        .method = HTTP_PUT,
        .handler = guarded_api_layout_put,
        .user_ctx = NULL,
    };
    httpd_uri_t get_entities = {
        .uri = "/api/entities",
        .method = HTTP_GET,
        .handler = guarded_api_entities_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_light_entities = {
        .uri = "/api/ha/light_entities",
        .method = HTTP_GET,
        .handler = guarded_api_light_entities_get,
        .user_ctx = NULL,
    };
    httpd_uri_t delete_light_entities = {
        .uri = "/api/ha/light_entities",
        .method = HTTP_DELETE,
        .handler = guarded_api_light_entities_delete,
        .user_ctx = NULL,
    };
    httpd_uri_t get_ha_energy = {
        .uri = "/api/ha/energy",
        .method = HTTP_GET,
        .handler = guarded_api_ha_energy_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_state = {
        .uri = "/api/state",
        .method = HTTP_GET,
        .handler = guarded_api_state_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_settings = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = guarded_api_settings_get,
        .user_ctx = NULL,
    };
    httpd_uri_t put_settings = {
        .uri = "/api/settings",
        .method = HTTP_PUT,
        .handler = guarded_api_settings_put,
        .user_ctx = NULL,
    };
    httpd_uri_t get_sd_status = {
        .uri = "/api/sd",
        .method = HTTP_GET,
        .handler = guarded_api_sd_status_get,
        .user_ctx = NULL,
    };
    /* Alias kept for clients that expect the /api/sd/status spelling (the 10"
     * Guiton build answers both); without it those polls log a 404 warning
     * every few seconds. */
    httpd_uri_t get_sd_status_alias = {
        .uri = "/api/sd/status",
        .method = HTTP_GET,
        .handler = guarded_api_sd_status_get,
        .user_ctx = NULL,
    };
    httpd_uri_t put_sd_status = {
        .uri = "/api/sd",
        .method = HTTP_PUT,
        .handler = guarded_api_sd_status_put,
        .user_ctx = NULL,
    };
    httpd_uri_t post_sd_format = {
        .uri = "/api/sd/format",
        .method = HTTP_POST,
        .handler = guarded_api_sd_format_post,
        .user_ctx = NULL,
    };
    httpd_uri_t get_sd_files = {
        .uri = "/api/sd/files",
        .method = HTTP_GET,
        .handler = guarded_api_sd_files_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_sd_file = {
        .uri = "/api/sd/file",
        .method = HTTP_GET,
        .handler = guarded_api_sd_file_get,
        .user_ctx = NULL,
    };
    httpd_uri_t delete_sd_file = {
        .uri = "/api/sd/file",
        .method = HTTP_DELETE,
        .handler = guarded_api_sd_file_delete,
        .user_ctx = NULL,
    };
    httpd_uri_t post_sd_logs_export = {
        .uri = "/api/sd/logs/export",
        .method = HTTP_POST,
        .handler = guarded_api_sd_logs_export_post,
        .user_ctx = NULL,
    };
    httpd_uri_t post_display_activity = {
        .uri = "/api/display/activity",
        .method = HTTP_POST,
        .handler = guarded_api_display_activity_post,
        .user_ctx = NULL,
    };
    httpd_uri_t post_display_wallpaper = {
        .uri = "/api/display/wallpaper",
        .method = HTTP_POST,
        .handler = guarded_api_display_wallpaper_post,
        .user_ctx = NULL,
    };
    httpd_uri_t get_display_wallpaper = {
        .uri = "/api/display/wallpaper",
        .method = HTTP_GET,
        .handler = guarded_api_display_wallpaper_get,
        .user_ctx = NULL,
    };
    httpd_uri_t delete_display_wallpaper = {
        .uri = "/api/display/wallpaper",
        .method = HTTP_DELETE,
        .handler = guarded_api_display_wallpaper_delete,
        .user_ctx = NULL,
    };
    httpd_uri_t get_i18n_languages = {
        .uri = "/api/i18n/languages",
        .method = HTTP_GET,
        .handler = guarded_api_i18n_languages_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_i18n_effective = {
        .uri = "/api/i18n/effective",
        .method = HTTP_GET,
        .handler = guarded_api_i18n_effective_get,
        .user_ctx = NULL,
    };
    httpd_uri_t put_i18n_custom = {
        .uri = "/api/i18n/custom",
        .method = HTTP_PUT,
        .handler = guarded_api_i18n_custom_put,
        .user_ctx = NULL,
    };
    httpd_uri_t get_wifi_scan = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = guarded_api_wifi_scan_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_version = {
        .uri = "/api/version",
        .method = HTTP_GET,
        .handler = guarded_api_version_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_screenshot_bmp = {
        .uri = "/api/screenshot.bmp",
        .method = HTTP_GET,
        .handler = guarded_api_screenshot_bmp_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_ota_status = {
        .uri = "/api/ota/status",
        .method = HTTP_GET,
        .handler = guarded_api_ota_status_get,
        .user_ctx = NULL,
    };
    httpd_uri_t post_ota_url = {
        .uri = "/api/ota/url",
        .method = HTTP_POST,
        .handler = guarded_api_ota_url_post,
        .user_ctx = NULL,
    };
    httpd_uri_t post_ota_upload = {
        .uri = "/api/ota/upload",
        .method = HTTP_POST,
        .handler = guarded_api_ota_upload_post,
        .user_ctx = NULL,
    };

    httpd_uri_t get_themes_list = {
        .uri = "/api/themes",
        .method = HTTP_GET,
        .handler = guarded_api_themes_list_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_themes_active = {
        .uri = "/api/themes/active",
        .method = HTTP_GET,
        .handler = guarded_api_themes_active_get,
        .user_ctx = NULL,
    };
    httpd_uri_t put_themes_active = {
        .uri = "/api/themes/active",
        .method = HTTP_PUT,
        .handler = guarded_api_themes_active_put,
        .user_ctx = NULL,
    };
    httpd_uri_t get_themes_export = {
        .uri = "/api/themes/get",
        .method = HTTP_GET,
        .handler = guarded_api_themes_get,
        .user_ctx = NULL,
    };
    httpd_uri_t put_themes_custom = {
        .uri = "/api/themes/custom",
        .method = HTTP_PUT,
        .handler = guarded_api_themes_custom_put,
        .user_ctx = NULL,
    };
    httpd_uri_t delete_themes_custom = {
        .uri = "/api/themes/custom",
        .method = HTTP_DELETE,
        .handler = guarded_api_themes_custom_delete,
        .user_ctx = NULL,
    };

    httpd_uri_t get_ha_diagnostics = {
        .uri = "/api/ha/diagnostics",
        .method = HTTP_GET,
        .handler = guarded_api_ha_diagnostics_get,
        .user_ctx = NULL,
    };

    httpd_uri_t get_cameras = {
        .uri = "/api/cameras",
        .method = HTTP_GET,
        .handler = guarded_api_cameras_get,
        .user_ctx = NULL,
    };
    httpd_uri_t put_cameras = {
        .uri = "/api/cameras",
        .method = HTTP_PUT,
        .handler = guarded_api_cameras_put,
        .user_ctx = NULL,
    };

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    httpd_uri_t get_camera_local_snapshot = {
        .uri = "/api/camera/snapshot",
        .method = HTTP_GET,
        .handler = guarded_api_camera_local_snapshot_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_camera_local_status = {
        .uri = "/api/camera/status",
        .method = HTTP_GET,
        .handler = guarded_api_camera_local_status_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_camera_local_motion = {
        .uri = "/api/camera/motion",
        .method = HTTP_GET,
        .handler = guarded_api_camera_local_motion_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_camera_local_stream = {
        .uri = "/api/camera/stream",
        .method = HTTP_GET,
        .handler = guarded_api_camera_local_stream_get,
        .user_ctx = NULL,
    };
#endif

    httpd_uri_t get_diagnostics = {
        .uri = "/api/diagnostics",
        .method = HTTP_GET,
        .handler = guarded_api_diagnostics_get,
        .user_ctx = NULL,
    };

    httpd_uri_t get_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = guarded_api_status_get,
        .user_ctx = NULL,
    };

    httpd_uri_t get_logs = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = guarded_api_logs_get,
        .user_ctx = NULL,
    };
    httpd_uri_t delete_logs = {
        .uri = "/api/logs",
        .method = HTTP_DELETE,
        .handler = guarded_api_logs_delete,
        .user_ctx = NULL,
    };

    httpd_uri_t get_crash = {
        .uri = "/api/crash",
        .method = HTTP_GET,
        .handler = guarded_api_crash_get,
        .user_ctx = NULL,
    };
    httpd_uri_t get_crash_raw = {
        .uri = "/api/crash/raw",
        .method = HTTP_GET,
        .handler = guarded_api_crash_raw_get,
        .user_ctx = NULL,
    };
    httpd_uri_t post_crash_erase = {
        .uri = "/api/crash/erase",
        .method = HTTP_POST,
        .handler = guarded_api_crash_erase_post,
        .user_ctx = NULL,
    };

    httpd_uri_t get_backup = {
        .uri = "/api/backup",
        .method = HTTP_GET,
        .handler = guarded_api_backup_get,
        .user_ctx = NULL,
    };

    httpd_uri_t post_backup_restore = {
        .uri = "/api/backup/restore",
        .method = HTTP_POST,
        .handler = guarded_api_backup_restore_post,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_layout), "api_routes", "GET /api/layout");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &put_layout), "api_routes", "PUT /api/layout");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_entities), "api_routes", "GET /api/entities");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_light_entities), "api_routes", "GET /api/ha/light_entities");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &delete_light_entities), "api_routes", "DELETE /api/ha/light_entities");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_ha_energy), "api_routes", "GET /api/ha/energy");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_state), "api_routes", "GET /api/state");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_settings), "api_routes", "GET /api/settings");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &put_settings), "api_routes", "PUT /api/settings");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_sd_status), "api_routes", "GET /api/sd");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_sd_status_alias), "api_routes", "GET /api/sd/status");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &put_sd_status), "api_routes", "PUT /api/sd");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post_sd_format), "api_routes", "POST /api/sd/format");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_sd_files), "api_routes", "GET /api/sd/files");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_sd_file), "api_routes", "GET /api/sd/file");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &delete_sd_file), "api_routes", "DELETE /api/sd/file");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &post_sd_logs_export), "api_routes", "POST /api/sd/logs/export");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &post_display_activity), "api_routes", "POST /api/display/activity");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &post_display_wallpaper), "api_routes", "POST /api/display/wallpaper");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_display_wallpaper), "api_routes", "GET /api/display/wallpaper");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &delete_display_wallpaper), "api_routes", "DELETE /api/display/wallpaper");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_i18n_languages), "api_routes", "GET /api/i18n/languages");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_i18n_effective), "api_routes", "GET /api/i18n/effective");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &put_i18n_custom), "api_routes", "PUT /api/i18n/custom");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_wifi_scan), "api_routes", "GET /api/wifi/scan");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_version), "api_routes", "GET /api/version");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_screenshot_bmp), "api_routes", "GET /api/screenshot.bmp");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_ota_status), "api_routes", "GET /api/ota/status");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post_ota_url), "api_routes", "POST /api/ota/url");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post_ota_upload), "api_routes", "POST /api/ota/upload");

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_themes_list), "api_routes", "GET /api/themes");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_themes_active), "api_routes", "GET /api/themes/active");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &put_themes_active), "api_routes", "PUT /api/themes/active");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_themes_export), "api_routes", "GET /api/themes/get");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &put_themes_custom), "api_routes", "PUT /api/themes/custom");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &delete_themes_custom), "api_routes", "DELETE /api/themes/custom");

    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_ha_diagnostics), "api_routes", "GET /api/ha/diagnostics");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_cameras), "api_routes", "GET /api/cameras");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &put_cameras), "api_routes", "PUT /api/cameras");
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_camera_local_snapshot), "api_routes", "GET /api/camera/snapshot");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_camera_local_status), "api_routes", "GET /api/camera/status");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_camera_local_motion), "api_routes", "GET /api/camera/motion");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_camera_local_stream), "api_routes", "GET /api/camera/stream");
#endif
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_logs), "api_routes", "GET /api/logs");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &delete_logs), "api_routes", "DELETE /api/logs");

    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &get_diagnostics), "api_routes", "GET /api/diagnostics");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_status), "api_routes", "GET /api/status");

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_crash), "api_routes", "GET /api/crash");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_crash_raw), "api_routes", "GET /api/crash/raw");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &post_crash_erase), "api_routes", "POST /api/crash/erase");

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_backup), "api_routes", "GET /api/backup");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &post_backup_restore), "api_routes", "POST /api/backup/restore");

    httpd_uri_t get_pages = {
        .uri = "/api/pages",
        .method = HTTP_GET,
        .handler = guarded_api_pages_get,
        .user_ctx = NULL,
    };
    httpd_uri_t post_pages_activate = {
        .uri = "/api/pages/activate",
        .method = HTTP_POST,
        .handler = guarded_api_pages_activate_post,
        .user_ctx = NULL,
    };
    /* TEMPORARY debug route (see api_radio_get_handler). */
    httpd_uri_t get_radio_debug = {
        .uri = "/api/radio",
        .method = HTTP_GET,
        .handler = guarded_api_radio_debug_get,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_radio_debug), "api_routes", "GET /api/radio");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_pages), "api_routes", "GET /api/pages");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server, &post_pages_activate), "api_routes", "POST /api/pages/activate");

    return ESP_OK;
}
