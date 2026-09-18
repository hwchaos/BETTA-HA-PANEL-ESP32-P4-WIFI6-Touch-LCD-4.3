/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "settings/runtime_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "app_config.h"
#include "diag/storage_guard.h"
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
#include "camera/local_camera.h"
#endif

#include "settings/i18n_store.h"
#include "util/log_tags.h"

#define SETTINGS_NVS_NAMESPACE "runtime_sec"
#define SETTINGS_NVS_KEY_WIFI_PASSWORD "wifi_pwd"
#define SETTINGS_NVS_KEY_HA_ACCESS_TOKEN "ha_token"
#define SETTINGS_NVS_KEY_XIAOZHI_TOKEN "xz_token"
#define SETTINGS_NVS_KEY_MQTT_PASSWORD "mqtt_pwd"

static bool is_placeholder(const char *text)
{
    return text == NULL || text[0] == '\0' || strstr(text, "YOUR_") != NULL;
}

static void normalize_ui_language(char *language, size_t language_len)
{
    if (language == NULL || language_len == 0) {
        return;
    }
    if (language[0] == '\0') {
        strlcpy(language, APP_UI_DEFAULT_LANGUAGE, language_len);
        return;
    }

    char normalized[APP_UI_LANGUAGE_MAX_LEN] = {0};
    if (!i18n_store_normalize_language_code(language, normalized, sizeof(normalized))) {
        strlcpy(language, APP_UI_DEFAULT_LANGUAGE, language_len);
        return;
    }
    strlcpy(language, normalized, language_len);
}

/* Accepted page transition names; the value is stored as a string so new
 * animations can be added without touching the settings format. */
static const char *const PAGE_TRANSITION_NAMES[] = {"none", "fade", "slide", "slide_up", "fade_slide"};

bool runtime_settings_page_transition_from_string(const char *text, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    if (text == NULL || text[0] == '\0') {
        strlcpy(out, APP_DISPLAY_PAGE_TRANSITION_DEFAULT, out_len);
        return true;
    }
    for (size_t i = 0; i < sizeof(PAGE_TRANSITION_NAMES) / sizeof(PAGE_TRANSITION_NAMES[0]); i++) {
        if (strcasecmp(text, PAGE_TRANSITION_NAMES[i]) == 0) {
            strlcpy(out, PAGE_TRANSITION_NAMES[i], out_len);
            return true;
        }
    }
    return false;
}

const char *const *runtime_settings_page_transition_names(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(PAGE_TRANSITION_NAMES) / sizeof(PAGE_TRANSITION_NAMES[0]);
    }
    return PAGE_TRANSITION_NAMES;
}

/* Accepted tap-feedback names; stored as a string so new effects can be added
 * without touching the settings format. */
static const char *const TILE_PRESS_FX_NAMES[] = {"none", "dim", "scale", "both"};

bool runtime_settings_tile_press_fx_from_string(const char *text, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    if (text == NULL || text[0] == '\0') {
        strlcpy(out, APP_TILE_PRESS_FX_DEFAULT, out_len);
        return true;
    }
    for (size_t i = 0; i < sizeof(TILE_PRESS_FX_NAMES) / sizeof(TILE_PRESS_FX_NAMES[0]); i++) {
        if (strcasecmp(text, TILE_PRESS_FX_NAMES[i]) == 0) {
            strlcpy(out, TILE_PRESS_FX_NAMES[i], out_len);
            return true;
        }
    }
    return false;
}

const char *const *runtime_settings_tile_press_fx_names(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(TILE_PRESS_FX_NAMES) / sizeof(TILE_PRESS_FX_NAMES[0]);
    }
    return TILE_PRESS_FX_NAMES;
}

/* Accepted value-animation names; stored as a string so new effects can be
 * added without touching the settings format. */
static const char *const VALUE_ANIM_NAMES[] = {"none", "fade", "slide", "count"};

bool runtime_settings_value_anim_from_string(const char *text, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    if (text == NULL || text[0] == '\0') {
        strlcpy(out, APP_DISPLAY_VALUE_ANIM_DEFAULT, out_len);
        return true;
    }
    for (size_t i = 0; i < sizeof(VALUE_ANIM_NAMES) / sizeof(VALUE_ANIM_NAMES[0]); i++) {
        if (strcasecmp(text, VALUE_ANIM_NAMES[i]) == 0) {
            strlcpy(out, VALUE_ANIM_NAMES[i], out_len);
            return true;
        }
    }
    return false;
}

const char *const *runtime_settings_value_anim_names(size_t *count)
{
    if (count != NULL) {
        *count = sizeof(VALUE_ANIM_NAMES) / sizeof(VALUE_ANIM_NAMES[0]);
    }
    return VALUE_ANIM_NAMES;
}

static void json_copy_string(cJSON *obj, const char *key, char *dst, size_t dst_len)
{
    if (obj == NULL || key == NULL || dst == NULL || dst_len == 0) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(dst, item->valuestring, dst_len);
    }
}

static void json_copy_int(cJSON *obj, const char *key, int *dst, int min, int max)
{
    if (obj == NULL || key == NULL || dst == NULL) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        int value = (int)item->valuedouble;
        if (value < min) {
            value = min;
        }
        if (value > max) {
            value = max;
        }
        *dst = value;
    }
}

static void json_copy_bool(cJSON *obj, const char *key, bool *dst)
{
    if (obj == NULL || key == NULL || dst == NULL) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

static esp_err_t load_file_text(const char *path, size_t max_len, char **out_text)
{
    if (path == NULL || out_text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > max_len) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    char *buf = calloc((size_t)size + 1U, sizeof(char));
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1U, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }

    *out_text = buf;
    return ESP_OK;
}

/* Builds the public settings document (no passwords/tokens) used both for the
 * on-flash settings.json and for backup downloads. Caller owns the result. */
cJSON *runtime_settings_public_json(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON *ha = cJSON_CreateObject();
    cJSON *time_cfg = cJSON_CreateObject();
    cJSON *ui = cJSON_CreateObject();
    cJSON *xiaozhi = cJSON_CreateObject();
    cJSON *display = cJSON_CreateObject();
    cJSON *mqtt = cJSON_CreateObject();
    cJSON *system = cJSON_CreateObject();
    cJSON *storage = cJSON_CreateObject();
    if (root == NULL || wifi == NULL || ha == NULL || time_cfg == NULL || ui == NULL || xiaozhi == NULL ||
        display == NULL || mqtt == NULL || system == NULL || storage == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(wifi);
        cJSON_Delete(ha);
        cJSON_Delete(time_cfg);
        cJSON_Delete(ui);
        cJSON_Delete(xiaozhi);
        cJSON_Delete(display);
        cJSON_Delete(mqtt);
        cJSON_Delete(system);
        cJSON_Delete(storage);
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", 1);

    cJSON_AddStringToObject(wifi, "ssid", settings->wifi_ssid);
    cJSON_AddStringToObject(wifi, "country_code", settings->wifi_country_code);
    cJSON_AddStringToObject(wifi, "bssid", settings->wifi_bssid);
    cJSON_AddBoolToObject(wifi, "static_enabled", settings->wifi_static_enabled);
    cJSON_AddStringToObject(wifi, "static_ip", settings->wifi_static_ip);
    cJSON_AddStringToObject(wifi, "static_netmask", settings->wifi_static_netmask);
    cJSON_AddStringToObject(wifi, "static_gateway", settings->wifi_static_gateway);
    cJSON_AddStringToObject(wifi, "static_dns", settings->wifi_static_dns);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON_AddStringToObject(ha, "ws_url", settings->ha_ws_url);
    cJSON_AddBoolToObject(ha, "rest_enabled", settings->ha_rest_enabled);
    cJSON_AddItemToObject(root, "ha", ha);

    cJSON_AddStringToObject(time_cfg, "ntp_server", settings->ntp_server);
    cJSON_AddStringToObject(time_cfg, "timezone", settings->time_tz);
    cJSON_AddItemToObject(root, "time", time_cfg);

    cJSON_AddStringToObject(ui, "language", settings->ui_language);
    cJSON_AddItemToObject(root, "ui", ui);

    /* The Xiaozhi token is a secret and is persisted in NVS, never here. */
    cJSON_AddStringToObject(xiaozhi, "server", settings->xiaozhi_server);
    cJSON_AddStringToObject(xiaozhi, "device", settings->xiaozhi_device);
    cJSON_AddStringToObject(xiaozhi, "ota_url", settings->xiaozhi_ota_url);
    cJSON_AddBoolToObject(xiaozhi, "enabled", settings->xiaozhi_enabled);
    cJSON_AddItemToObject(root, "xiaozhi", xiaozhi);

    cJSON_AddNumberToObject(display, "brightness", settings->display_brightness);
    cJSON_AddNumberToObject(display, "saver_brightness", settings->display_saver_brightness);
    cJSON_AddBoolToObject(display, "screensaver_enabled", settings->display_screensaver_enabled);
    cJSON_AddNumberToObject(display, "screensaver_timeout_sec", (double)settings->display_screensaver_timeout_sec);
    cJSON_AddBoolToObject(display, "screen_off_enabled", settings->display_screen_off_enabled);
    cJSON_AddNumberToObject(display, "screen_off_timeout_sec", (double)settings->display_screen_off_timeout_sec);
    cJSON_AddBoolToObject(display, "clock_24h", settings->display_clock_24h);
    cJSON_AddBoolToObject(display, "saver_show_seconds", settings->display_saver_show_seconds);
    cJSON_AddBoolToObject(display, "saver_show_date", settings->display_saver_show_date);
    cJSON_AddNumberToObject(display, "saver_clock_style", settings->display_saver_clock_style);
    cJSON_AddNumberToObject(display, "saver_wallpaper_dim", settings->display_saver_wallpaper_dim);
    cJSON_AddNumberToObject(display, "saver_clock_color", (double)(settings->display_saver_clock_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "saver_date_color", (double)(settings->display_saver_date_color & 0xFFFFFF));
    cJSON_AddBoolToObject(display, "night_mode_enabled", settings->display_night_mode_enabled);
    cJSON_AddNumberToObject(display, "night_start_min", (double)settings->display_night_start_min);
    cJSON_AddNumberToObject(display, "night_end_min", (double)settings->display_night_end_min);
    cJSON_AddNumberToObject(display, "night_brightness", (double)settings->display_night_brightness);
    cJSON_AddNumberToObject(display, "night_wake_sec", (double)settings->display_night_wake_sec);
    cJSON_AddBoolToObject(display, "theme_auto_enabled", settings->display_theme_auto_enabled);
    cJSON_AddStringToObject(display, "theme_day_id", settings->display_theme_day_id);
    cJSON_AddStringToObject(display, "theme_night_id", settings->display_theme_night_id);
    cJSON_AddStringToObject(display, "page_transition", settings->display_page_transition);
    cJSON_AddNumberToObject(display, "page_transition_ms", (double)settings->display_page_transition_ms);
    cJSON_AddStringToObject(display, "tile_press_fx", settings->display_tile_press_fx);
    cJSON_AddNumberToObject(display, "tile_press_fx_dim", (double)settings->display_tile_press_fx_dim);
    cJSON_AddNumberToObject(display, "tile_press_fx_scale", (double)settings->display_tile_press_fx_scale);
    cJSON_AddStringToObject(display, "value_anim", settings->display_value_anim);
    cJSON_AddNumberToObject(display, "value_anim_ms", (double)settings->display_value_anim_ms);
    cJSON_AddBoolToObject(display, "topbar_show_clock", settings->display_topbar_show_clock);
    cJSON_AddBoolToObject(display, "topbar_show_date", settings->display_topbar_show_date);
    cJSON_AddBoolToObject(display, "topbar_show_gear", settings->display_topbar_show_gear);
    cJSON_AddBoolToObject(display, "topbar_show_status", settings->display_topbar_show_status);
    cJSON_AddBoolToObject(display, "topbar_icon_text", settings->display_topbar_icon_text);
    cJSON_AddBoolToObject(display, "topbar_custom_colors", settings->display_topbar_custom_colors);
    cJSON_AddNumberToObject(display, "topbar_bg_color", (double)(settings->display_topbar_bg_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "topbar_clock_color", (double)(settings->display_topbar_clock_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "topbar_date_color", (double)(settings->display_topbar_date_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "topbar_gear_color", (double)(settings->display_topbar_gear_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "topbar_ha_color", (double)(settings->display_topbar_ha_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "topbar_wifi_color", (double)(settings->display_topbar_wifi_color & 0xFFFFFF));
    cJSON_AddBoolToObject(display, "nav_custom_colors", settings->display_nav_custom_colors);
    cJSON_AddNumberToObject(display, "nav_bar_bg_color", (double)(settings->display_nav_bar_bg_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "nav_bar_border_color", (double)(settings->display_nav_bar_border_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "nav_button_bg_color", (double)(settings->display_nav_button_bg_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "nav_button_border_color", (double)(settings->display_nav_button_border_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "nav_tab_idle_color", (double)(settings->display_nav_tab_idle_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "nav_tab_active_color", (double)(settings->display_nav_tab_active_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "nav_home_idle_color", (double)(settings->display_nav_home_idle_color & 0xFFFFFF));
    cJSON_AddNumberToObject(display, "nav_home_active_color", (double)(settings->display_nav_home_active_color & 0xFFFFFF));
    cJSON_AddItemToObject(root, "display", display);

    cJSON_AddBoolToObject(mqtt, "enabled", settings->mqtt_enabled);
    cJSON_AddBoolToObject(mqtt, "use_tls", settings->mqtt_use_tls);
    cJSON_AddStringToObject(mqtt, "host", settings->mqtt_host);
    cJSON_AddNumberToObject(mqtt, "port", (double)settings->mqtt_port);
    cJSON_AddStringToObject(mqtt, "username", settings->mqtt_username);
    cJSON_AddStringToObject(mqtt, "discovery_prefix", settings->mqtt_discovery_prefix);
    cJSON_AddItemToObject(root, "mqtt", mqtt);

    cJSON_AddBoolToObject(system, "auto_restart_enabled", settings->system_auto_restart_enabled);
    cJSON_AddNumberToObject(system, "auto_restart_hours", (double)settings->system_auto_restart_hours);
    /* 0 = off, 1 = errors, 2 = +warnings, 3 = +info, 4 = +debug. */
    cJSON_AddNumberToObject(system, "log_verbosity", (double)settings->log_verbosity);
    cJSON_AddItemToObject(root, "system", system);

    cJSON_AddBoolToObject(storage, "sd_enabled", settings->sd_enabled);
    cJSON_AddItemToObject(root, "storage", storage);

    /* Built-in MIPI-CSI camera: stream tuning plus the motion detector setup. */
    cJSON *camera = cJSON_CreateObject();
    cJSON *camera_motion = cJSON_CreateObject();
    cJSON *motion_zones = cJSON_CreateArray();
    if (camera != NULL && camera_motion != NULL && motion_zones != NULL) {
        cJSON_AddBoolToObject(camera, "enabled", settings->camera_enabled);
        cJSON_AddBoolToObject(camera, "motion_wake", settings->camera_motion_wake);
        cJSON_AddNumberToObject(camera, "motion_threshold", settings->camera_motion_threshold);
        cJSON_AddNumberToObject(camera, "jpeg_quality", settings->camera_jpeg_quality);
        cJSON_AddBoolToObject(camera, "hflip", settings->camera_hflip);
        cJSON_AddBoolToObject(camera, "vflip", settings->camera_vflip);
        cJSON_AddBoolToObject(camera, "stream_enabled", settings->camera_stream_enabled);
        cJSON_AddNumberToObject(camera, "resolution", settings->camera_resolution);

        cJSON_AddNumberToObject(camera_motion, "min_area", settings->camera_motion_min_area);
        cJSON_AddNumberToObject(camera_motion, "min_duration_ms", settings->camera_motion_min_duration_ms);
        cJSON_AddNumberToObject(camera_motion, "cooldown_ms", settings->camera_motion_cooldown_ms);
        cJSON_AddNumberToObject(camera_motion, "start_delay_ms", settings->camera_motion_start_delay_ms);
        cJSON_AddBoolToObject(camera_motion, "ignore_lighting", settings->camera_motion_ignore_lighting);
        for (int i = 0; i < settings->camera_motion_zone_count && i < RUNTIME_MOTION_MAX_ZONES; i++) {
            cJSON *zone = cJSON_CreateObject();
            if (zone == NULL) {
                break;
            }
            cJSON_AddNumberToObject(zone, "x", settings->camera_motion_zones[i].x);
            cJSON_AddNumberToObject(zone, "y", settings->camera_motion_zones[i].y);
            cJSON_AddNumberToObject(zone, "w", settings->camera_motion_zones[i].w);
            cJSON_AddNumberToObject(zone, "h", settings->camera_motion_zones[i].h);
            cJSON_AddItemToArray(motion_zones, zone);
        }
        cJSON_AddItemToObject(camera_motion, "zones", motion_zones);
        cJSON_AddItemToObject(camera, "motion", camera_motion);
        cJSON_AddItemToObject(root, "camera", camera);
    } else {
        cJSON_Delete(camera);
        cJSON_Delete(camera_motion);
        cJSON_Delete(motion_zones);
    }

    return root;
}

static esp_err_t write_public_settings_file(const runtime_settings_t *settings)
{
    cJSON *root = runtime_settings_public_json(settings);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(APP_SETTINGS_PATH, "wb");
    if (f == NULL) {
        cJSON_free(payload);
        return ESP_FAIL;
    }

    size_t len = strlen(payload);
    size_t written = fwrite(payload, 1U, len, f);
    fclose(f);
    cJSON_free(payload);

    if (written != len) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t parse_settings_json(
    const char *json,
    runtime_settings_t *out,
    bool *out_legacy_wifi_password,
    bool *out_legacy_ha_access_token,
    bool *out_legacy_mqtt_password)
{
    if (json == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_legacy_wifi_password != NULL) {
        *out_legacy_wifi_password = false;
    }
    if (out_legacy_ha_access_token != NULL) {
        *out_legacy_ha_access_token = false;
    }
    if (out_legacy_mqtt_password != NULL) {
        *out_legacy_mqtt_password = false;
    }

    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    cJSON *ha = cJSON_GetObjectItemCaseSensitive(root, "ha");
    cJSON *time_cfg = cJSON_GetObjectItemCaseSensitive(root, "time");
    cJSON *ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
    cJSON *xiaozhi = cJSON_GetObjectItemCaseSensitive(root, "xiaozhi");
    cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
    cJSON *storage = cJSON_GetObjectItemCaseSensitive(root, "storage");
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(root, "camera");

    if (cJSON_IsObject(wifi)) {
        json_copy_string(wifi, "ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
        json_copy_string(wifi, "country_code", out->wifi_country_code, sizeof(out->wifi_country_code));
        json_copy_string(wifi, "bssid", out->wifi_bssid, sizeof(out->wifi_bssid));
        cJSON *static_enabled = cJSON_GetObjectItemCaseSensitive(wifi, "static_enabled");
        if (cJSON_IsBool(static_enabled)) {
            out->wifi_static_enabled = cJSON_IsTrue(static_enabled);
        }
        json_copy_string(wifi, "static_ip", out->wifi_static_ip, sizeof(out->wifi_static_ip));
        json_copy_string(wifi, "static_netmask", out->wifi_static_netmask, sizeof(out->wifi_static_netmask));
        json_copy_string(wifi, "static_gateway", out->wifi_static_gateway, sizeof(out->wifi_static_gateway));
        json_copy_string(wifi, "static_dns", out->wifi_static_dns, sizeof(out->wifi_static_dns));
        cJSON *pwd = cJSON_GetObjectItemCaseSensitive(wifi, "password");
        if (pwd != NULL) {
            if (out_legacy_wifi_password != NULL) {
                *out_legacy_wifi_password = true;
            }
            if (cJSON_IsString(pwd) && pwd->valuestring != NULL) {
                strlcpy(out->wifi_password, pwd->valuestring, sizeof(out->wifi_password));
            }
        }
    } else {
        json_copy_string(root, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
        json_copy_string(root, "wifi_country_code", out->wifi_country_code, sizeof(out->wifi_country_code));
        json_copy_string(root, "wifi_bssid", out->wifi_bssid, sizeof(out->wifi_bssid));
        cJSON *static_enabled = cJSON_GetObjectItemCaseSensitive(root, "wifi_static_enabled");
        if (cJSON_IsBool(static_enabled)) {
            out->wifi_static_enabled = cJSON_IsTrue(static_enabled);
        }
        json_copy_string(root, "wifi_static_ip", out->wifi_static_ip, sizeof(out->wifi_static_ip));
        json_copy_string(root, "wifi_static_netmask", out->wifi_static_netmask, sizeof(out->wifi_static_netmask));
        json_copy_string(root, "wifi_static_gateway", out->wifi_static_gateway, sizeof(out->wifi_static_gateway));
        json_copy_string(root, "wifi_static_dns", out->wifi_static_dns, sizeof(out->wifi_static_dns));
        cJSON *pwd = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
        if (pwd != NULL) {
            if (out_legacy_wifi_password != NULL) {
                *out_legacy_wifi_password = true;
            }
            if (cJSON_IsString(pwd) && pwd->valuestring != NULL) {
                strlcpy(out->wifi_password, pwd->valuestring, sizeof(out->wifi_password));
            }
        }
    }

    if (cJSON_IsObject(ha)) {
        json_copy_string(ha, "ws_url", out->ha_ws_url, sizeof(out->ha_ws_url));
        cJSON *rest_enabled = cJSON_GetObjectItemCaseSensitive(ha, "rest_enabled");
        if (cJSON_IsBool(rest_enabled)) {
            out->ha_rest_enabled = cJSON_IsTrue(rest_enabled);
        }
        cJSON *token = cJSON_GetObjectItemCaseSensitive(ha, "access_token");
        if (token != NULL) {
            if (out_legacy_ha_access_token != NULL) {
                *out_legacy_ha_access_token = true;
            }
            if (cJSON_IsString(token) && token->valuestring != NULL) {
                strlcpy(out->ha_access_token, token->valuestring, sizeof(out->ha_access_token));
            }
        }
    } else {
        json_copy_string(root, "ha_ws_url", out->ha_ws_url, sizeof(out->ha_ws_url));
        cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "ha_access_token");
        if (token != NULL) {
            if (out_legacy_ha_access_token != NULL) {
                *out_legacy_ha_access_token = true;
            }
            if (cJSON_IsString(token) && token->valuestring != NULL) {
                strlcpy(out->ha_access_token, token->valuestring, sizeof(out->ha_access_token));
            }
        }
    }
    cJSON *rest_enabled = cJSON_GetObjectItemCaseSensitive(root, "ha_rest_enabled");
    if (cJSON_IsBool(rest_enabled)) {
        out->ha_rest_enabled = cJSON_IsTrue(rest_enabled);
    }

    if (cJSON_IsObject(time_cfg)) {
        json_copy_string(time_cfg, "ntp_server", out->ntp_server, sizeof(out->ntp_server));
        json_copy_string(time_cfg, "timezone", out->time_tz, sizeof(out->time_tz));
    } else {
        json_copy_string(root, "ntp_server", out->ntp_server, sizeof(out->ntp_server));
        json_copy_string(root, "time_tz", out->time_tz, sizeof(out->time_tz));
    }

    if (cJSON_IsObject(ui)) {
        json_copy_string(ui, "language", out->ui_language, sizeof(out->ui_language));
    } else {
        json_copy_string(root, "language", out->ui_language, sizeof(out->ui_language));
    }

    if (cJSON_IsObject(xiaozhi)) {
        json_copy_string(xiaozhi, "server", out->xiaozhi_server, sizeof(out->xiaozhi_server));
        json_copy_string(xiaozhi, "device", out->xiaozhi_device, sizeof(out->xiaozhi_device));
        json_copy_string(xiaozhi, "ota_url", out->xiaozhi_ota_url, sizeof(out->xiaozhi_ota_url));
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(xiaozhi, "enabled");
        if (cJSON_IsBool(enabled)) {
            out->xiaozhi_enabled = cJSON_IsTrue(enabled);
        }
    } else {
        json_copy_string(root, "xiaozhi_server", out->xiaozhi_server, sizeof(out->xiaozhi_server));
        json_copy_string(root, "xiaozhi_device", out->xiaozhi_device, sizeof(out->xiaozhi_device));
        json_copy_string(root, "xiaozhi_ota_url", out->xiaozhi_ota_url, sizeof(out->xiaozhi_ota_url));
    }

    if (cJSON_IsObject(display)) {
        cJSON *brightness = cJSON_GetObjectItemCaseSensitive(display, "brightness");
        if (cJSON_IsNumber(brightness)) {
            int value = (int)brightness->valuedouble;
            if (value < 0) {
                value = 0;
            }
            if (value > 100) {
                value = 100;
            }
            out->display_brightness = (uint8_t)value;
        }
        cJSON *saver_brightness = cJSON_GetObjectItemCaseSensitive(display, "saver_brightness");
        if (cJSON_IsNumber(saver_brightness)) {
            int value = (int)saver_brightness->valuedouble;
            if (value < 1) {
                value = 1;
            }
            if (value > APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT) {
                /* Values stored above the cap (older builds, the web UI before
                 * the cap, MQTT) are normalised on load so the panel and the
                 * reported settings agree on what the saver really uses. */
                value = APP_DISPLAY_SAVER_BRIGHTNESS_MAX_PERCENT;
            }
            out->display_saver_brightness = (uint8_t)value;
        }
        cJSON *screensaver_enabled = cJSON_GetObjectItemCaseSensitive(display, "screensaver_enabled");
        if (cJSON_IsBool(screensaver_enabled)) {
            out->display_screensaver_enabled = cJSON_IsTrue(screensaver_enabled);
        }
        cJSON *screensaver_timeout = cJSON_GetObjectItemCaseSensitive(display, "screensaver_timeout_sec");
        if (cJSON_IsNumber(screensaver_timeout)) {
            double value = screensaver_timeout->valuedouble;
            if (value < 0) {
                value = 0;
            }
            out->display_screensaver_timeout_sec = (uint32_t)value;
        }
        cJSON *screen_off_enabled = cJSON_GetObjectItemCaseSensitive(display, "screen_off_enabled");
        if (cJSON_IsBool(screen_off_enabled)) {
            out->display_screen_off_enabled = cJSON_IsTrue(screen_off_enabled);
        }
        cJSON *screen_off_timeout = cJSON_GetObjectItemCaseSensitive(display, "screen_off_timeout_sec");
        if (cJSON_IsNumber(screen_off_timeout)) {
            double value = screen_off_timeout->valuedouble;
            if (value < 0) {
                value = 0;
            }
            out->display_screen_off_timeout_sec = (uint32_t)value;
        }
        cJSON *clock_24h = cJSON_GetObjectItemCaseSensitive(display, "clock_24h");
        if (cJSON_IsBool(clock_24h)) {
            out->display_clock_24h = cJSON_IsTrue(clock_24h);
        }
        cJSON *saver_show_seconds = cJSON_GetObjectItemCaseSensitive(display, "saver_show_seconds");
        if (cJSON_IsBool(saver_show_seconds)) {
            out->display_saver_show_seconds = cJSON_IsTrue(saver_show_seconds);
        }
        cJSON *saver_show_date = cJSON_GetObjectItemCaseSensitive(display, "saver_show_date");
        if (cJSON_IsBool(saver_show_date)) {
            out->display_saver_show_date = cJSON_IsTrue(saver_show_date);
        }
        cJSON *saver_clock_style = cJSON_GetObjectItemCaseSensitive(display, "saver_clock_style");
        if (cJSON_IsNumber(saver_clock_style)) {
            int value = saver_clock_style->valueint;
            if (value < APP_DISPLAY_SAVER_CLOCK_STYLE_CLASSIC) {
                value = APP_DISPLAY_SAVER_CLOCK_STYLE_CLASSIC;
            }
            if (value > APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP) {
                value = APP_DISPLAY_SAVER_CLOCK_STYLE_FLIP;
            }
            out->display_saver_clock_style = (uint8_t)value;
        }
        cJSON *saver_wallpaper_dim = cJSON_GetObjectItemCaseSensitive(display, "saver_wallpaper_dim");
        if (cJSON_IsNumber(saver_wallpaper_dim)) {
            int value = (int)saver_wallpaper_dim->valuedouble;
            if (value < 0) {
                value = 0;
            }
            if (value > APP_DISPLAY_SAVER_WALLPAPER_DIM_MAX) {
                value = APP_DISPLAY_SAVER_WALLPAPER_DIM_MAX;
            }
            out->display_saver_wallpaper_dim = (uint8_t)value;
        }
        cJSON *saver_clock_color = cJSON_GetObjectItemCaseSensitive(display, "saver_clock_color");
        if (cJSON_IsNumber(saver_clock_color)) {
            uint32_t value = (uint32_t)saver_clock_color->valuedouble;
            if (value > 0xFFFFFF) {
                value = 0xFFFFFF;
            }
            out->display_saver_clock_color = value;
        }
        cJSON *saver_date_color = cJSON_GetObjectItemCaseSensitive(display, "saver_date_color");
        if (cJSON_IsNumber(saver_date_color)) {
            uint32_t value = (uint32_t)saver_date_color->valuedouble;
            if (value > 0xFFFFFF) {
                value = 0xFFFFFF;
            }
            out->display_saver_date_color = value;
        }

        cJSON *night_enabled = cJSON_GetObjectItemCaseSensitive(display, "night_mode_enabled");
        if (cJSON_IsBool(night_enabled)) {
            out->display_night_mode_enabled = cJSON_IsTrue(night_enabled);
        }
        cJSON *night_start = cJSON_GetObjectItemCaseSensitive(display, "night_start_min");
        if (cJSON_IsNumber(night_start)) {
            uint16_t value = (uint16_t)night_start->valuedouble;
            out->display_night_start_min = (value > 1439U) ? 1439U : value;
        }
        cJSON *night_end = cJSON_GetObjectItemCaseSensitive(display, "night_end_min");
        if (cJSON_IsNumber(night_end)) {
            uint16_t value = (uint16_t)night_end->valuedouble;
            out->display_night_end_min = (value > 1439U) ? 1439U : value;
        }
        cJSON *night_brightness = cJSON_GetObjectItemCaseSensitive(display, "night_brightness");
        if (cJSON_IsNumber(night_brightness)) {
            double value = night_brightness->valuedouble;
            if (value < 0) {
                value = 0;
            }
            if (value > 100) {
                value = 100;
            }
            out->display_night_brightness = (uint8_t)value;
        }
        cJSON *night_wake = cJSON_GetObjectItemCaseSensitive(display, "night_wake_sec");
        if (cJSON_IsNumber(night_wake)) {
            double value = night_wake->valuedouble;
            if (value < 0) {
                value = 0;
            }
            if (value > 3600) {
                value = 3600;
            }
            out->display_night_wake_sec = (uint16_t)value;
        }
        cJSON *theme_auto = cJSON_GetObjectItemCaseSensitive(display, "theme_auto_enabled");
        if (cJSON_IsBool(theme_auto)) {
            out->display_theme_auto_enabled = cJSON_IsTrue(theme_auto);
        }
        cJSON *theme_day = cJSON_GetObjectItemCaseSensitive(display, "theme_day_id");
        if (cJSON_IsString(theme_day) && theme_day->valuestring != NULL) {
            snprintf(out->display_theme_day_id, sizeof(out->display_theme_day_id), "%s", theme_day->valuestring);
        }
        cJSON *theme_night = cJSON_GetObjectItemCaseSensitive(display, "theme_night_id");
        if (cJSON_IsString(theme_night) && theme_night->valuestring != NULL) {
            snprintf(out->display_theme_night_id, sizeof(out->display_theme_night_id), "%s", theme_night->valuestring);
        }
        cJSON *page_transition = cJSON_GetObjectItemCaseSensitive(display, "page_transition");
        if (cJSON_IsString(page_transition) && page_transition->valuestring != NULL) {
            char transition[APP_DISPLAY_PAGE_TRANSITION_MAX_LEN] = {0};
            /* Unknown names keep the current value instead of failing the load. */
            if (runtime_settings_page_transition_from_string(page_transition->valuestring, transition,
                                                             sizeof(transition))) {
                strlcpy(out->display_page_transition, transition, sizeof(out->display_page_transition));
            }
        }
        cJSON *page_transition_ms = cJSON_GetObjectItemCaseSensitive(display, "page_transition_ms");
        if (cJSON_IsNumber(page_transition_ms)) {
            double value = page_transition_ms->valuedouble;
            if (value < 0) {
                value = 0;
            }
            if (value > APP_DISPLAY_PAGE_TRANSITION_MAX_MS) {
                value = APP_DISPLAY_PAGE_TRANSITION_MAX_MS;
            }
            out->display_page_transition_ms = (uint16_t)value;
        }
        cJSON *tile_press_fx = cJSON_GetObjectItemCaseSensitive(display, "tile_press_fx");
        if (cJSON_IsString(tile_press_fx) && tile_press_fx->valuestring != NULL) {
            char press_fx[APP_TILE_PRESS_FX_MAX_LEN] = {0};
            /* Unknown names keep the current value instead of failing the load. */
            if (runtime_settings_tile_press_fx_from_string(tile_press_fx->valuestring, press_fx,
                                                           sizeof(press_fx))) {
                strlcpy(out->display_tile_press_fx, press_fx, sizeof(out->display_tile_press_fx));
            }
        }
        cJSON *press_fx_dim = cJSON_GetObjectItemCaseSensitive(display, "tile_press_fx_dim");
        if (cJSON_IsNumber(press_fx_dim)) {
            double value = press_fx_dim->valuedouble;
            if (value < 0) {
                value = 0;
            }
            if (value > APP_TILE_PRESS_FX_DIM_MAX) {
                value = APP_TILE_PRESS_FX_DIM_MAX;
            }
            out->display_tile_press_fx_dim = (uint8_t)value;
        }
        cJSON *press_fx_scale = cJSON_GetObjectItemCaseSensitive(display, "tile_press_fx_scale");
        if (cJSON_IsNumber(press_fx_scale)) {
            double value = press_fx_scale->valuedouble;
            if (value < APP_TILE_PRESS_FX_SCALE_MIN) {
                value = APP_TILE_PRESS_FX_SCALE_MIN;
            }
            if (value > APP_TILE_PRESS_FX_SCALE_MAX) {
                value = APP_TILE_PRESS_FX_SCALE_MAX;
            }
            out->display_tile_press_fx_scale = (uint8_t)value;
        }
        cJSON *value_anim = cJSON_GetObjectItemCaseSensitive(display, "value_anim");
        if (cJSON_IsString(value_anim) && value_anim->valuestring != NULL) {
            char anim[APP_DISPLAY_VALUE_ANIM_MAX_LEN] = {0};
            /* Unknown names keep the current value instead of failing the load. */
            if (runtime_settings_value_anim_from_string(value_anim->valuestring, anim, sizeof(anim))) {
                strlcpy(out->display_value_anim, anim, sizeof(out->display_value_anim));
            }
        }
        cJSON *value_anim_ms = cJSON_GetObjectItemCaseSensitive(display, "value_anim_ms");
        if (cJSON_IsNumber(value_anim_ms)) {
            double value = value_anim_ms->valuedouble;
            if (value < 0) {
                value = 0;
            }
            if (value > APP_DISPLAY_VALUE_ANIM_MAX_MS) {
                value = APP_DISPLAY_VALUE_ANIM_MAX_MS;
            }
            out->display_value_anim_ms = (uint16_t)value;
        }
        cJSON *topbar_show_clock = cJSON_GetObjectItemCaseSensitive(display, "topbar_show_clock");
        if (cJSON_IsBool(topbar_show_clock)) {
            out->display_topbar_show_clock = cJSON_IsTrue(topbar_show_clock);
        }
        cJSON *topbar_show_date = cJSON_GetObjectItemCaseSensitive(display, "topbar_show_date");
        if (cJSON_IsBool(topbar_show_date)) {
            out->display_topbar_show_date = cJSON_IsTrue(topbar_show_date);
        }
        cJSON *topbar_show_gear = cJSON_GetObjectItemCaseSensitive(display, "topbar_show_gear");
        if (cJSON_IsBool(topbar_show_gear)) {
            out->display_topbar_show_gear = cJSON_IsTrue(topbar_show_gear);
        }
        cJSON *topbar_show_status = cJSON_GetObjectItemCaseSensitive(display, "topbar_show_status");
        if (cJSON_IsBool(topbar_show_status)) {
            out->display_topbar_show_status = cJSON_IsTrue(topbar_show_status);
        }
        cJSON *topbar_icon_text = cJSON_GetObjectItemCaseSensitive(display, "topbar_icon_text");
        if (cJSON_IsBool(topbar_icon_text)) {
            out->display_topbar_icon_text = cJSON_IsTrue(topbar_icon_text);
        }
        cJSON *topbar_custom_colors = cJSON_GetObjectItemCaseSensitive(display, "topbar_custom_colors");
        if (cJSON_IsBool(topbar_custom_colors)) {
            out->display_topbar_custom_colors = cJSON_IsTrue(topbar_custom_colors);
        }
        struct {
            const char *key;
            uint32_t *target;
        } topbar_colors[] = {
            {"topbar_bg_color", &out->display_topbar_bg_color},
            {"topbar_clock_color", &out->display_topbar_clock_color},
            {"topbar_date_color", &out->display_topbar_date_color},
            {"topbar_gear_color", &out->display_topbar_gear_color},
            {"topbar_ha_color", &out->display_topbar_ha_color},
            {"topbar_wifi_color", &out->display_topbar_wifi_color},
        };
        for (size_t i = 0; i < sizeof(topbar_colors) / sizeof(topbar_colors[0]); i++) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(display, topbar_colors[i].key);
            if (cJSON_IsNumber(item)) {
                double value = item->valuedouble;
                if (value < 0) {
                    value = 0;
                }
                if (value > 0xFFFFFF) {
                    value = 0xFFFFFF;
                }
                *topbar_colors[i].target = (uint32_t)value;
            }
        }
        cJSON *nav_custom_colors = cJSON_GetObjectItemCaseSensitive(display, "nav_custom_colors");
        if (cJSON_IsBool(nav_custom_colors)) {
            out->display_nav_custom_colors = cJSON_IsTrue(nav_custom_colors);
        }
        struct {
            const char *key;
            uint32_t *target;
        } nav_colors[] = {
            {"nav_bar_bg_color", &out->display_nav_bar_bg_color},
            {"nav_bar_border_color", &out->display_nav_bar_border_color},
            {"nav_button_bg_color", &out->display_nav_button_bg_color},
            {"nav_button_border_color", &out->display_nav_button_border_color},
            {"nav_tab_idle_color", &out->display_nav_tab_idle_color},
            {"nav_tab_active_color", &out->display_nav_tab_active_color},
            {"nav_home_idle_color", &out->display_nav_home_idle_color},
            {"nav_home_active_color", &out->display_nav_home_active_color},
        };
        for (size_t i = 0; i < sizeof(nav_colors) / sizeof(nav_colors[0]); i++) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(display, nav_colors[i].key);
            if (cJSON_IsNumber(item)) {
                double value = item->valuedouble;
                if (value < 0) {
                    value = 0;
                }
                if (value > 0xFFFFFF) {
                    value = 0xFFFFFF;
                }
                *nav_colors[i].target = (uint32_t)value;
            }
        }
    }

    if (cJSON_IsObject(mqtt)) {
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(mqtt, "enabled");
        if (cJSON_IsBool(enabled)) {
            out->mqtt_enabled = cJSON_IsTrue(enabled);
        }
        json_copy_string(mqtt, "host", out->mqtt_host, sizeof(out->mqtt_host));
        cJSON *use_tls = cJSON_GetObjectItemCaseSensitive(mqtt, "use_tls");
        if (cJSON_IsBool(use_tls)) {
            out->mqtt_use_tls = cJSON_IsTrue(use_tls);
        }
        cJSON *port = cJSON_GetObjectItemCaseSensitive(mqtt, "port");
        if (cJSON_IsNumber(port)) {
            int value = (int)port->valuedouble;
            if (value < 1) {
                value = 1;
            }
            if (value > 65535) {
                value = 65535;
            }
            out->mqtt_port = (uint16_t)value;
        }
        json_copy_string(mqtt, "username", out->mqtt_username, sizeof(out->mqtt_username));
        json_copy_string(mqtt, "discovery_prefix", out->mqtt_discovery_prefix, sizeof(out->mqtt_discovery_prefix));
        cJSON *password = cJSON_GetObjectItemCaseSensitive(mqtt, "password");
        if (password != NULL) {
            if (out_legacy_mqtt_password != NULL) {
                *out_legacy_mqtt_password = true;
            }
            if (cJSON_IsString(password) && password->valuestring != NULL) {
                strlcpy(out->mqtt_password, password->valuestring, sizeof(out->mqtt_password));
            }
        }
    }

    if (cJSON_IsObject(system)) {
        cJSON *auto_restart_enabled = cJSON_GetObjectItemCaseSensitive(system, "auto_restart_enabled");
        if (cJSON_IsBool(auto_restart_enabled)) {
            out->system_auto_restart_enabled = cJSON_IsTrue(auto_restart_enabled);
        }
        cJSON *auto_restart_hours = cJSON_GetObjectItemCaseSensitive(system, "auto_restart_hours");
        if (cJSON_IsNumber(auto_restart_hours)) {
            int value = (int)auto_restart_hours->valuedouble;
            if (value < 1) {
                value = 1;
            }
            if (value > 168) {
                value = 168;
            }
            out->system_auto_restart_hours = (uint32_t)value;
        }
        cJSON *log_verbosity = cJSON_GetObjectItemCaseSensitive(system, "log_verbosity");
        if (cJSON_IsNumber(log_verbosity)) {
            int value = (int)log_verbosity->valuedouble;
            if (value < 0) {
                value = 0;
            }
            if (value > 5) {
                value = 5;
            }
            out->log_verbosity = value;
        }
    }

    if (cJSON_IsObject(storage)) {
        cJSON *sd_enabled = cJSON_GetObjectItemCaseSensitive(storage, "sd_enabled");
        if (cJSON_IsBool(sd_enabled)) {
            out->sd_enabled = cJSON_IsTrue(sd_enabled);
        }
    }

    if (cJSON_IsObject(camera)) {
        json_copy_bool(camera, "enabled", &out->camera_enabled);
        json_copy_bool(camera, "motion_wake", &out->camera_motion_wake);
        json_copy_int(camera, "motion_threshold", &out->camera_motion_threshold, 1, 64);
        json_copy_int(camera, "jpeg_quality", &out->camera_jpeg_quality, 10, 95);
        json_copy_bool(camera, "hflip", &out->camera_hflip);
        json_copy_bool(camera, "vflip", &out->camera_vflip);
        json_copy_bool(camera, "stream_enabled", &out->camera_stream_enabled);
        json_copy_int(camera, "resolution", &out->camera_resolution, 0, 1);

        cJSON *motion = cJSON_GetObjectItemCaseSensitive(camera, "motion");
        if (cJSON_IsObject(motion)) {
            json_copy_int(motion, "min_area", &out->camera_motion_min_area, 0, 100);
            json_copy_int(motion, "min_duration_ms", &out->camera_motion_min_duration_ms, 0, 1000);
            json_copy_int(motion, "cooldown_ms", &out->camera_motion_cooldown_ms, 0, 30000);
            json_copy_int(motion, "start_delay_ms", &out->camera_motion_start_delay_ms, 0, 10000);
            json_copy_bool(motion, "ignore_lighting", &out->camera_motion_ignore_lighting);

            cJSON *zones = cJSON_GetObjectItemCaseSensitive(motion, "zones");
            if (cJSON_IsArray(zones)) {
                int count = 0;
                const int total = cJSON_GetArraySize(zones);
                for (int i = 0; i < total && count < RUNTIME_MOTION_MAX_ZONES; i++) {
                    cJSON *zone = cJSON_GetArrayItem(zones, i);
                    if (!cJSON_IsObject(zone)) {
                        continue;
                    }
                    int x = 0, y = 0, w = 0, h = 0;
                    json_copy_int(zone, "x", &x, 0, 100);
                    json_copy_int(zone, "y", &y, 0, 100);
                    json_copy_int(zone, "w", &w, 0, 100);
                    json_copy_int(zone, "h", &h, 0, 100);
                    if (w <= 0 || h <= 0) {
                        continue;
                    }
                    out->camera_motion_zones[count].x = x;
                    out->camera_motion_zones[count].y = y;
                    out->camera_motion_zones[count].w = w;
                    out->camera_motion_zones[count].h = h;
                    count++;
                }
                out->camera_motion_zone_count = count;
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t nvs_load_secret(const char *key, char *out, size_t out_len, bool *out_found)
{
    if (key == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (out_found != NULL) {
        *out_found = false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required = 0;
    err = nvs_get_str(handle, key, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (required == 0) {
        if (out_found != NULL) {
            *out_found = true;
        }
        nvs_close(handle);
        return ESP_OK;
    }
    if (required > out_len) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_str(handle, key, out, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    if (out_found != NULL) {
        *out_found = true;
    }
    return ESP_OK;
}

static esp_err_t nvs_set_or_erase_secret(nvs_handle_t handle, const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (value[0] == '\0') {
        esp_err_t err = nvs_erase_key(handle, key);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;
        }
        return err;
    }

    return nvs_set_str(handle, key, value);
}

static esp_err_t nvs_save_secrets(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_or_erase_secret(handle, SETTINGS_NVS_KEY_WIFI_PASSWORD, settings->wifi_password);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_or_erase_secret(handle, SETTINGS_NVS_KEY_HA_ACCESS_TOKEN, settings->ha_access_token);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_or_erase_secret(handle, SETTINGS_NVS_KEY_XIAOZHI_TOKEN, settings->xiaozhi_token);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_or_erase_secret(handle, SETTINGS_NVS_KEY_MQTT_PASSWORD, settings->mqtt_password);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    storage_guard_flash_op_begin(FLASH_OP_NVS);
    err = nvs_commit(handle);
    storage_guard_flash_op_end(FLASH_OP_NVS);
    nvs_close(handle);
    return err;
}

static void runtime_settings_merge_secrets_from_nvs(runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    char wifi_password[APP_WIFI_PASSWORD_MAX_LEN] = {0};
    bool has_wifi_password = false;
    esp_err_t wifi_err =
        nvs_load_secret(SETTINGS_NVS_KEY_WIFI_PASSWORD, wifi_password, sizeof(wifi_password), &has_wifi_password);
    if (wifi_err == ESP_OK && has_wifi_password) {
        strlcpy(settings->wifi_password, wifi_password, sizeof(settings->wifi_password));
    }

    char ha_access_token[APP_HA_ACCESS_TOKEN_MAX_LEN] = {0};
    bool has_ha_access_token = false;
    esp_err_t token_err = nvs_load_secret(
        SETTINGS_NVS_KEY_HA_ACCESS_TOKEN, ha_access_token, sizeof(ha_access_token), &has_ha_access_token);
    if (token_err == ESP_OK && has_ha_access_token) {
        strlcpy(settings->ha_access_token, ha_access_token, sizeof(settings->ha_access_token));
    }

    char xiaozhi_token[APP_XIAOZHI_TOKEN_MAX_LEN] = {0};
    bool has_xiaozhi_token = false;
    esp_err_t xz_token_err = nvs_load_secret(
        SETTINGS_NVS_KEY_XIAOZHI_TOKEN, xiaozhi_token, sizeof(xiaozhi_token), &has_xiaozhi_token);
    if (xz_token_err == ESP_OK && has_xiaozhi_token) {
        strlcpy(settings->xiaozhi_token, xiaozhi_token, sizeof(settings->xiaozhi_token));
    }

    char mqtt_password[APP_MQTT_PASSWORD_MAX_LEN] = {0};
    bool has_mqtt_password = false;
    esp_err_t mqtt_err = nvs_load_secret(
        SETTINGS_NVS_KEY_MQTT_PASSWORD, mqtt_password, sizeof(mqtt_password), &has_mqtt_password);
    if (mqtt_err == ESP_OK && has_mqtt_password) {
        strlcpy(settings->mqtt_password, mqtt_password, sizeof(settings->mqtt_password));
    }
}

void runtime_settings_set_defaults(runtime_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (!is_placeholder(APP_WIFI_SSID)) {
        strlcpy(out->wifi_ssid, APP_WIFI_SSID, sizeof(out->wifi_ssid));
    }
    if (!is_placeholder(APP_WIFI_PASSWORD)) {
        strlcpy(out->wifi_password, APP_WIFI_PASSWORD, sizeof(out->wifi_password));
    }
    strlcpy(out->wifi_country_code, APP_WIFI_COUNTRY_CODE, sizeof(out->wifi_country_code));
    if (!is_placeholder(APP_HA_WS_URL)) {
        strlcpy(out->ha_ws_url, APP_HA_WS_URL, sizeof(out->ha_ws_url));
    }
    if (!is_placeholder(APP_HA_ACCESS_TOKEN)) {
        strlcpy(out->ha_access_token, APP_HA_ACCESS_TOKEN, sizeof(out->ha_access_token));
    }
    out->ha_rest_enabled = false;
    strlcpy(out->ntp_server, APP_NTP_SERVER, sizeof(out->ntp_server));
    strlcpy(out->time_tz, APP_TIME_TZ, sizeof(out->time_tz));
    strlcpy(out->ui_language, APP_UI_DEFAULT_LANGUAGE, sizeof(out->ui_language));

    out->xiaozhi_server[0] = '\0';
    out->xiaozhi_device[0] = '\0';
    out->xiaozhi_token[0] = '\0';
    strlcpy(out->xiaozhi_ota_url, APP_XIAOZHI_OTA_URL_DEFAULT, sizeof(out->xiaozhi_ota_url));
    out->xiaozhi_enabled = false;
    out->display_brightness = APP_DISPLAY_ACTIVE_BRIGHTNESS_PERCENT;
    out->display_saver_brightness = APP_DISPLAY_SAVER_BRIGHTNESS_PERCENT;
    out->display_screensaver_enabled = true;
    out->display_screensaver_timeout_sec = APP_DISPLAY_SCREENSAVER_TIMEOUT_SEC;
    out->display_screen_off_enabled = APP_DISPLAY_SCREEN_OFF_ENABLED ? true : false;
    out->display_screen_off_timeout_sec = APP_DISPLAY_SCREEN_OFF_TIMEOUT_SEC;
    out->display_clock_24h = APP_DISPLAY_CLOCK_24H;
    out->display_saver_show_seconds = APP_DISPLAY_SAVER_SHOW_SECONDS;
    out->display_saver_show_date = APP_DISPLAY_SAVER_SHOW_DATE;
    out->display_saver_clock_style = APP_DISPLAY_SAVER_CLOCK_STYLE_DEFAULT;
    out->display_saver_wallpaper_dim = APP_DISPLAY_SAVER_WALLPAPER_DIM_PERCENT;
    out->display_saver_clock_color = APP_DISPLAY_SAVER_CLOCK_COLOR;
    out->display_saver_date_color = APP_DISPLAY_SAVER_DATE_COLOR;
    out->display_night_mode_enabled = APP_DISPLAY_NIGHT_MODE_ENABLED ? true : false;
    out->display_night_start_min = APP_DISPLAY_NIGHT_START_MIN;
    out->display_night_end_min = APP_DISPLAY_NIGHT_END_MIN;
    out->display_night_brightness = APP_DISPLAY_NIGHT_BRIGHTNESS_PERCENT;
    out->display_night_wake_sec = APP_DISPLAY_NIGHT_WAKE_SEC;
    out->display_theme_auto_enabled = false;
    out->display_theme_day_id[0] = '\0';
    out->display_theme_night_id[0] = '\0';
    strlcpy(out->display_page_transition, APP_DISPLAY_PAGE_TRANSITION_DEFAULT,
            sizeof(out->display_page_transition));
    out->display_page_transition_ms = APP_DISPLAY_PAGE_TRANSITION_DEFAULT_MS;
    strlcpy(out->display_value_anim, APP_DISPLAY_VALUE_ANIM_DEFAULT, sizeof(out->display_value_anim));
    out->display_value_anim_ms = APP_DISPLAY_VALUE_ANIM_DEFAULT_MS;
    out->display_topbar_show_clock = APP_DISPLAY_TOPBAR_SHOW_CLOCK;
    out->display_topbar_show_date = APP_DISPLAY_TOPBAR_SHOW_DATE;
    out->display_topbar_show_gear = APP_DISPLAY_TOPBAR_SHOW_GEAR;
    out->display_topbar_show_status = APP_DISPLAY_TOPBAR_SHOW_STATUS;
    out->display_topbar_icon_text = APP_DISPLAY_TOPBAR_ICON_TEXT;
    out->display_topbar_custom_colors = APP_DISPLAY_TOPBAR_CUSTOM_COLORS;
    out->display_topbar_bg_color = APP_DISPLAY_TOPBAR_BG_COLOR;
    out->display_topbar_clock_color = APP_DISPLAY_TOPBAR_CLOCK_COLOR;
    out->display_topbar_date_color = APP_DISPLAY_TOPBAR_DATE_COLOR;
    out->display_topbar_gear_color = APP_DISPLAY_TOPBAR_GEAR_COLOR;
    out->display_topbar_ha_color = APP_DISPLAY_TOPBAR_HA_COLOR;
    out->display_topbar_wifi_color = APP_DISPLAY_TOPBAR_WIFI_COLOR;
    out->display_nav_custom_colors = APP_DISPLAY_NAV_CUSTOM_COLORS;
    out->display_nav_bar_bg_color = APP_DISPLAY_NAV_BAR_BG_COLOR;
    out->display_nav_bar_border_color = APP_DISPLAY_NAV_BAR_BORDER_COLOR;
    out->display_nav_button_bg_color = APP_DISPLAY_NAV_BUTTON_BG_COLOR;
    out->display_nav_button_border_color = APP_DISPLAY_NAV_BUTTON_BORDER_COLOR;
    out->display_nav_tab_idle_color = APP_DISPLAY_NAV_TAB_IDLE_COLOR;
    out->display_nav_tab_active_color = APP_DISPLAY_NAV_TAB_ACTIVE_COLOR;
    out->display_nav_home_idle_color = APP_DISPLAY_NAV_HOME_IDLE_COLOR;
    out->display_nav_home_active_color = APP_DISPLAY_NAV_HOME_ACTIVE_COLOR;
    strlcpy(out->display_tile_press_fx, APP_TILE_PRESS_FX_DEFAULT, sizeof(out->display_tile_press_fx));
    out->display_tile_press_fx_dim = APP_TILE_PRESS_FX_DIM_DEFAULT;
    out->display_tile_press_fx_scale = APP_TILE_PRESS_FX_SCALE_DEFAULT;
    out->mqtt_enabled = APP_MQTT_ENABLED_DEFAULT;
    out->mqtt_use_tls = false;
    out->mqtt_host[0] = '\0';
    out->mqtt_port = APP_MQTT_PORT_DEFAULT;
    out->mqtt_username[0] = '\0';
    out->mqtt_password[0] = '\0';
    strlcpy(out->mqtt_discovery_prefix, APP_MQTT_DISCOVERY_PREFIX_DEFAULT, sizeof(out->mqtt_discovery_prefix));
    out->system_auto_restart_enabled = false;
    out->system_auto_restart_hours = 24;
    /* VERBOSE by default (see APP_LOG_VERBOSITY_DEFAULT): the log should show
     * everything the panel is doing, not only the lines that happened to be
     * errors, so a rare screen flash or stall can be read back afterwards. */
    out->log_verbosity = APP_LOG_VERBOSITY_DEFAULT;
    out->sd_enabled = true;

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    out->camera_enabled = false;
    out->camera_motion_wake = CONFIG_APP_LOCAL_CAMERA_MOTION_WAKE;
    out->camera_motion_threshold = 8;
    out->camera_jpeg_quality = CONFIG_APP_LOCAL_CAMERA_JPEG_QUALITY;
#ifdef CONFIG_APP_LOCAL_CAMERA_HFLIP
    out->camera_hflip = CONFIG_APP_LOCAL_CAMERA_HFLIP;
#else
    out->camera_hflip = false;
#endif
#ifdef CONFIG_APP_LOCAL_CAMERA_VFLIP
    out->camera_vflip = CONFIG_APP_LOCAL_CAMERA_VFLIP;
#else
    out->camera_vflip = false;
#endif
    out->camera_stream_enabled = false;
    out->camera_resolution = 0;
#else
    out->camera_enabled = false;
    out->camera_motion_wake = false;
    out->camera_motion_threshold = 8;
    out->camera_jpeg_quality = 55;
    out->camera_hflip = false;
    out->camera_vflip = false;
    out->camera_stream_enabled = false;
    out->camera_resolution = 0;
#endif

    /* Motion-detector tuning: whole frame, fire immediately, 1 s cooldown,
     * 2 s start grace period, global-lighting filter on. */
    out->camera_motion_min_area = 0;
    out->camera_motion_min_duration_ms = 0;
    out->camera_motion_cooldown_ms = 1000;
    out->camera_motion_start_delay_ms = 2000;
    out->camera_motion_ignore_lighting = true;
    out->camera_motion_zone_count = 0;
    for (int i = 0; i < RUNTIME_MOTION_MAX_ZONES; i++) {
        out->camera_motion_zones[i].x = 0;
        out->camera_motion_zones[i].y = 0;
        out->camera_motion_zones[i].w = 0;
        out->camera_motion_zones[i].h = 0;
    }
}

esp_err_t runtime_settings_load(runtime_settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    runtime_settings_set_defaults(out);

    char *json = NULL;
    esp_err_t file_err = load_file_text(APP_SETTINGS_PATH, APP_SETTINGS_MAX_JSON_LEN, &json);
    if (file_err != ESP_OK) {
        return file_err;
    }

    bool legacy_wifi_password = false;
    bool legacy_ha_access_token = false;
    bool legacy_mqtt_password = false;
    esp_err_t parse_err =
        parse_settings_json(json, out, &legacy_wifi_password, &legacy_ha_access_token, &legacy_mqtt_password);
    free(json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }
    if (out->wifi_country_code[0] == '\0') {
        strlcpy(out->wifi_country_code, APP_WIFI_COUNTRY_CODE, sizeof(out->wifi_country_code));
    }
    normalize_ui_language(out->ui_language, sizeof(out->ui_language));

    char nvs_wifi_password[APP_WIFI_PASSWORD_MAX_LEN] = {0};
    bool has_nvs_wifi_password = false;
    esp_err_t nvs_err =
        nvs_load_secret(SETTINGS_NVS_KEY_WIFI_PASSWORD, nvs_wifi_password, sizeof(nvs_wifi_password), &has_nvs_wifi_password);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }
    if (has_nvs_wifi_password) {
        strlcpy(out->wifi_password, nvs_wifi_password, sizeof(out->wifi_password));
    }

    char nvs_ha_access_token[APP_HA_ACCESS_TOKEN_MAX_LEN] = {0};
    bool has_nvs_ha_access_token = false;
    nvs_err = nvs_load_secret(
        SETTINGS_NVS_KEY_HA_ACCESS_TOKEN, nvs_ha_access_token, sizeof(nvs_ha_access_token), &has_nvs_ha_access_token);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }
    if (has_nvs_ha_access_token) {
        strlcpy(out->ha_access_token, nvs_ha_access_token, sizeof(out->ha_access_token));
    }

    char nvs_mqtt_password[APP_MQTT_PASSWORD_MAX_LEN] = {0};
    bool has_nvs_mqtt_password = false;
    nvs_err = nvs_load_secret(
        SETTINGS_NVS_KEY_MQTT_PASSWORD, nvs_mqtt_password, sizeof(nvs_mqtt_password), &has_nvs_mqtt_password);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }
    if (has_nvs_mqtt_password) {
        strlcpy(out->mqtt_password, nvs_mqtt_password, sizeof(out->mqtt_password));
    }

    bool migrate_to_nvs =
        (legacy_wifi_password && !has_nvs_wifi_password && out->wifi_password[0] != '\0') ||
        (legacy_ha_access_token && !has_nvs_ha_access_token && out->ha_access_token[0] != '\0') ||
        (legacy_mqtt_password && !has_nvs_mqtt_password && out->mqtt_password[0] != '\0');

    if (migrate_to_nvs) {
        nvs_err = nvs_save_secrets(out);
        if (nvs_err != ESP_OK) {
            return nvs_err;
        }
    }

    if (legacy_wifi_password || legacy_ha_access_token || legacy_mqtt_password) {
        esp_err_t scrub_err = write_public_settings_file(out);
        if (scrub_err != ESP_OK) {
            ESP_LOGW(TAG_APP, "Failed to scrub legacy secrets from LittleFS settings: %s", esp_err_to_name(scrub_err));
        }
    }

    return ESP_OK;
}

esp_err_t runtime_settings_save(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t file_err = write_public_settings_file(settings);
    if (file_err != ESP_OK) {
        return file_err;
    }

    esp_err_t nvs_err = nvs_save_secrets(settings);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }

    ESP_LOGI(TAG_APP, "Saved runtime settings");
    return ESP_OK;
}

static bool settings_connection_fields_equal(const runtime_settings_t *a, const runtime_settings_t *b)
{
    return strcmp(a->wifi_ssid, b->wifi_ssid) == 0 &&
           strcmp(a->wifi_country_code, b->wifi_country_code) == 0 &&
           strcmp(a->wifi_bssid, b->wifi_bssid) == 0 &&
           a->wifi_static_enabled == b->wifi_static_enabled &&
           strcmp(a->wifi_static_ip, b->wifi_static_ip) == 0 &&
           strcmp(a->wifi_static_netmask, b->wifi_static_netmask) == 0 &&
           strcmp(a->wifi_static_gateway, b->wifi_static_gateway) == 0 &&
           strcmp(a->wifi_static_dns, b->wifi_static_dns) == 0 &&
           strcmp(a->ha_ws_url, b->ha_ws_url) == 0 && a->ha_rest_enabled == b->ha_rest_enabled &&
           strcmp(a->ntp_server, b->ntp_server) == 0 && strcmp(a->time_tz, b->time_tz) == 0 &&
           a->mqtt_enabled == b->mqtt_enabled && a->mqtt_use_tls == b->mqtt_use_tls &&
           strcmp(a->mqtt_host, b->mqtt_host) == 0 && a->mqtt_port == b->mqtt_port &&
           strcmp(a->mqtt_username, b->mqtt_username) == 0 &&
           strcmp(a->mqtt_discovery_prefix, b->mqtt_discovery_prefix) == 0;
}

esp_err_t runtime_settings_validate_public_json(const char *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *probe = cJSON_Parse(json);
    if (!cJSON_IsObject(probe)) {
        cJSON_Delete(probe);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(probe);

    runtime_settings_t *candidate = calloc(1, sizeof(runtime_settings_t));
    if (candidate == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = runtime_settings_load(candidate);
    if (err == ESP_OK) {
        err = parse_settings_json(json, candidate, NULL, NULL, NULL);
    }

    free(candidate);
    return err;
}

esp_err_t runtime_settings_apply_public_json(const char *json, bool *restart_required)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (restart_required != NULL) {
        *restart_required = false;
    }

    cJSON *probe = cJSON_Parse(json);
    if (!cJSON_IsObject(probe)) {
        cJSON_Delete(probe);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(probe);

    runtime_settings_t *incoming = calloc(1, sizeof(runtime_settings_t));
    if (incoming == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = runtime_settings_load(incoming);
    if (err != ESP_OK) {
        free(incoming);
        return err;
    }

    runtime_settings_t before = *incoming;

    /* Credentials and Wi-Fi parameters are deliberately not restored: a wrong
     * SSID or password would leave the panel unreachable with no way back in. */
    err = parse_settings_json(json, incoming, NULL, NULL, NULL);
    if (err != ESP_OK) {
        free(incoming);
        return err;
    }

    strlcpy(incoming->wifi_ssid, before.wifi_ssid, sizeof(incoming->wifi_ssid));
    strlcpy(incoming->wifi_password, before.wifi_password, sizeof(incoming->wifi_password));
    strlcpy(incoming->wifi_country_code, before.wifi_country_code, sizeof(incoming->wifi_country_code));
    strlcpy(incoming->wifi_bssid, before.wifi_bssid, sizeof(incoming->wifi_bssid));
    incoming->wifi_static_enabled = before.wifi_static_enabled;
    strlcpy(incoming->wifi_static_ip, before.wifi_static_ip, sizeof(incoming->wifi_static_ip));
    strlcpy(incoming->wifi_static_netmask, before.wifi_static_netmask, sizeof(incoming->wifi_static_netmask));
    strlcpy(incoming->wifi_static_gateway, before.wifi_static_gateway, sizeof(incoming->wifi_static_gateway));
    strlcpy(incoming->wifi_static_dns, before.wifi_static_dns, sizeof(incoming->wifi_static_dns));
    strlcpy(incoming->ha_access_token, before.ha_access_token, sizeof(incoming->ha_access_token));
    strlcpy(incoming->mqtt_password, before.mqtt_password, sizeof(incoming->mqtt_password));

    err = runtime_settings_save(incoming);
    if (err == ESP_OK && restart_required != NULL) {
        *restart_required = !settings_connection_fields_equal(incoming, &before);
    }

    free(incoming);
    return err;
}

esp_err_t runtime_settings_init(void)
{
    runtime_settings_t *settings = calloc(1, sizeof(runtime_settings_t));
    if (settings == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = runtime_settings_load(settings);
    if (err == ESP_OK) {
        free(settings);
        return ESP_OK;
    }

    ESP_LOGW(TAG_APP, "Settings missing/invalid (%s), writing defaults", esp_err_to_name(err));
    runtime_settings_set_defaults(settings);
    runtime_settings_merge_secrets_from_nvs(settings);
    esp_err_t save_err = runtime_settings_save(settings);
    free(settings);
    return save_err;
}

bool runtime_settings_has_wifi(const runtime_settings_t *settings)
{
    return settings != NULL && settings->wifi_ssid[0] != '\0';
}

bool runtime_settings_has_ha(const runtime_settings_t *settings)
{
    return settings != NULL && settings->ha_ws_url[0] != '\0' && settings->ha_access_token[0] != '\0';
}

bool runtime_settings_has_xiaozhi(const runtime_settings_t *settings)
{
    return settings != NULL && settings->xiaozhi_enabled && settings->xiaozhi_server[0] != '\0';
}

esp_err_t runtime_settings_apply_motion_config(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    local_camera_motion_config_t config;
    memset(&config, 0, sizeof(config));
    config.threshold = (uint8_t)settings->camera_motion_threshold;
    config.min_area_pct = (uint8_t)settings->camera_motion_min_area;
    config.min_duration_ms = (uint16_t)settings->camera_motion_min_duration_ms;
    config.cooldown_ms = (uint16_t)settings->camera_motion_cooldown_ms;
    config.start_delay_ms = (uint16_t)settings->camera_motion_start_delay_ms;
    config.ignore_lighting = settings->camera_motion_ignore_lighting;
    config.zone_count = (uint8_t)settings->camera_motion_zone_count;
    for (int i = 0; i < LOCAL_CAMERA_MOTION_MAX_ZONES; i++) {
        config.zones[i].x = (uint8_t)settings->camera_motion_zones[i].x;
        config.zones[i].y = (uint8_t)settings->camera_motion_zones[i].y;
        config.zones[i].w = (uint8_t)settings->camera_motion_zones[i].w;
        config.zones[i].h = (uint8_t)settings->camera_motion_zones[i].h;
    }

    local_camera_set_motion_config(&config);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}
