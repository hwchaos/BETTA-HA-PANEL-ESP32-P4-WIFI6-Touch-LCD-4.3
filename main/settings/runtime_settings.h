/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"

#include "app_config.h"
#include "ui/theme/theme_palette.h"

/* Motion-detection zone in percent of the frame (0..100). */
#define RUNTIME_MOTION_MAX_ZONES 4

typedef struct {
    int x;
    int y;
    int w;
    int h;
} runtime_motion_zone_t;

typedef struct {
    char wifi_ssid[APP_WIFI_SSID_MAX_LEN];
    char wifi_password[APP_WIFI_PASSWORD_MAX_LEN];
    char wifi_country_code[APP_WIFI_COUNTRY_CODE_MAX_LEN];
    char wifi_bssid[APP_WIFI_BSSID_MAX_LEN];
    bool wifi_static_enabled;
    char wifi_static_ip[APP_WIFI_IPV4_MAX_LEN];
    char wifi_static_netmask[APP_WIFI_IPV4_MAX_LEN];
    char wifi_static_gateway[APP_WIFI_IPV4_MAX_LEN];
    char wifi_static_dns[APP_WIFI_IPV4_MAX_LEN];
    char ha_ws_url[APP_HA_WS_URL_MAX_LEN];
    char ha_access_token[APP_HA_ACCESS_TOKEN_MAX_LEN];
    bool ha_rest_enabled;
    char ntp_server[APP_NTP_SERVER_MAX_LEN];
    char time_tz[APP_TIME_TZ_MAX_LEN];
    char ui_language[APP_UI_LANGUAGE_MAX_LEN];
    char xiaozhi_server[APP_XIAOZHI_SERVER_MAX_LEN];
    char xiaozhi_device[APP_XIAOZHI_DEVICE_MAX_LEN];
    char xiaozhi_token[APP_XIAOZHI_TOKEN_MAX_LEN];
    char xiaozhi_ota_url[APP_XIAOZHI_OTA_URL_MAX_LEN];
    bool xiaozhi_enabled;
    uint8_t display_brightness;
    uint8_t display_saver_brightness;
    bool display_screensaver_enabled;
    uint32_t display_screensaver_timeout_sec;
    bool display_screen_off_enabled;
    uint32_t display_screen_off_timeout_sec;
    bool display_clock_24h;
    bool display_saver_show_seconds;
    bool display_saver_show_date;
    uint8_t display_saver_clock_style;  /* APP_DISPLAY_SAVER_CLOCK_STYLE_* */
    uint8_t display_saver_wallpaper_dim; /* darkening of the saver wallpaper, 0..APP_DISPLAY_SAVER_WALLPAPER_DIM_MAX */
    uint32_t display_saver_clock_color;
    uint32_t display_saver_date_color;
    bool display_night_mode_enabled;
    uint16_t display_night_start_min;  /* minutes since midnight, 0..1439 */
    uint16_t display_night_end_min;    /* minutes since midnight, 0..1439 */
    uint8_t display_night_brightness;  /* backlight percent inside the window, 0 = off */
    uint16_t display_night_wake_sec;   /* touch wake window inside the night schedule */
    /* Automatic day/night theme switching: inside the night window above the
     * panel renders the night theme, outside it the day theme. Empty ids mean
     * "keep the global theme". */
    bool display_theme_auto_enabled;
    char display_theme_day_id[APP_MAX_THEME_ID_LEN];
    char display_theme_night_id[APP_MAX_THEME_ID_LEN];
    char display_page_transition[APP_DISPLAY_PAGE_TRANSITION_MAX_LEN];
    uint16_t display_page_transition_ms;
    char display_tile_press_fx[APP_TILE_PRESS_FX_MAX_LEN];
    uint8_t display_tile_press_fx_dim;
    uint8_t display_tile_press_fx_scale;
    char display_value_anim[APP_DISPLAY_VALUE_ANIM_MAX_LEN];
    uint16_t display_value_anim_ms;
    bool display_topbar_show_clock;
    bool display_topbar_show_date;
    bool display_topbar_show_gear;
    bool display_topbar_show_status;
    bool display_topbar_icon_text;
    bool display_topbar_custom_colors;
    uint32_t display_topbar_bg_color;
    uint32_t display_topbar_clock_color;
    uint32_t display_topbar_date_color;
    uint32_t display_topbar_gear_color;
    uint32_t display_topbar_ha_color;
    uint32_t display_topbar_wifi_color;
    bool display_nav_custom_colors;
    uint32_t display_nav_bar_bg_color;
    uint32_t display_nav_bar_border_color;
    uint32_t display_nav_button_bg_color;
    uint32_t display_nav_button_border_color;
    uint32_t display_nav_tab_idle_color;
    uint32_t display_nav_tab_active_color;
    uint32_t display_nav_home_idle_color;
    uint32_t display_nav_home_active_color;
    bool mqtt_enabled;
    bool mqtt_use_tls;
    char mqtt_host[APP_MQTT_HOST_MAX_LEN];
    uint16_t mqtt_port;
    char mqtt_username[APP_MQTT_USERNAME_MAX_LEN];
    char mqtt_password[APP_MQTT_PASSWORD_MAX_LEN];
    char mqtt_discovery_prefix[APP_MQTT_DISCOVERY_PREFIX_MAX_LEN];
    bool system_auto_restart_enabled;
    uint32_t system_auto_restart_hours;
    /* Persistent log verbosity: 0 = off, 1 = errors, 2 = +warnings,
     * 3 = +info (default), 4 = +debug, 5 = +verbose (everything).
     * Applied to esp_log and to the capture filter at boot
     * (see system_log_set_verbosity()). */
    int log_verbosity;
    /* microSD / TF card.  When disabled the card is not even mounted, so no
     * FAT driver is pulled in and the panel behaves exactly like before. */
    bool sd_enabled;

    /* Built-in MIPI-CSI camera. */
    bool camera_enabled;
    bool camera_motion_wake;
    int camera_motion_threshold; /* 1..64; higher = less sensitive */
    int camera_jpeg_quality;     /* 10..95 */
    bool camera_hflip;
    bool camera_vflip;
    bool camera_stream_enabled; /* MJPEG live stream for HA (http://<ip>/api/camera/stream) */
    int camera_resolution;      /* 0 = native (1280x960), 1 = half (640x480) */

    /* Camera motion-detector tuning (zones + debounce + lighting filter). */
    int camera_motion_min_area;        /* 0..100 % of active cells, 0 = off */
    int camera_motion_min_duration_ms; /* 0..1000 ms, 0 = off */
    int camera_motion_cooldown_ms;     /* 0..30000 ms between two wake-ups */
    int camera_motion_start_delay_ms;  /* 0..10000 ms grace after start */
    bool camera_motion_ignore_lighting;
    int camera_motion_zone_count;      /* 0..4, 0 = whole frame */
    runtime_motion_zone_t camera_motion_zones[RUNTIME_MOTION_MAX_ZONES];
} runtime_settings_t;

void runtime_settings_set_defaults(runtime_settings_t *out);
esp_err_t runtime_settings_init(void);
esp_err_t runtime_settings_load(runtime_settings_t *out);
esp_err_t runtime_settings_save(const runtime_settings_t *settings);
/**
 * @brief Push the motion-detector tuning (zones, debounce, cooldown, filters)
 * into the camera component.  Safe to call before the camera is started; the
 * values persist in the component state.  No-op when local camera is disabled.
 */
esp_err_t runtime_settings_apply_motion_config(const runtime_settings_t *settings);
/* Returns the public (secret-free) settings document; caller owns it. */
cJSON *runtime_settings_public_json(const runtime_settings_t *settings);
/* Merges a public settings document back in, keeping local credentials.
 * Sets restart_required when connection-relevant fields changed. */
esp_err_t runtime_settings_apply_public_json(const char *json, bool *restart_required);
/* Parses a public settings document without touching the stored settings. */
esp_err_t runtime_settings_validate_public_json(const char *json);
/* Normalises a page transition name; empty input yields the default.
 * Returns false for unknown names, leaving `out` untouched. */
bool runtime_settings_page_transition_from_string(const char *text, char *out, size_t out_len);
/* Accepted transition names in canonical order; returns the array and, when
 * `count` is non-NULL, its length. */
const char *const *runtime_settings_page_transition_names(size_t *count);
/* Normalises a tap-feedback name; empty input yields the default. Returns false
 * for unknown names, leaving `out` untouched. */
bool runtime_settings_tile_press_fx_from_string(const char *text, char *out, size_t out_len);
/* Accepted tap-feedback names in canonical order; returns the array and, when
 * `count` is non-NULL, its length. */
const char *const *runtime_settings_tile_press_fx_names(size_t *count);
/* Normalises a value-animation name; empty input yields the default. Returns
 * false for unknown names, leaving `out` untouched. */
bool runtime_settings_value_anim_from_string(const char *text, char *out, size_t out_len);
/* Accepted value-animation names in canonical order; returns the array and,
 * when `count` is non-NULL, its length. */
const char *const *runtime_settings_value_anim_names(size_t *count);
bool runtime_settings_has_wifi(const runtime_settings_t *settings);
bool runtime_settings_has_ha(const runtime_settings_t *settings);
bool runtime_settings_has_xiaozhi(const runtime_settings_t *settings);
