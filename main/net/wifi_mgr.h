/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "app_config.h"

typedef struct {
    const char *ssid;
    const char *password;
    const char *country_code;
    uint8_t channel;
    uint8_t max_connection;
} wifi_mgr_ap_config_t;

typedef struct {
    char ssid[APP_WIFI_SSID_MAX_LEN];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
    uint8_t bssid[6];
    bool connected;
} wifi_mgr_scan_result_t;

typedef struct {
    const char *ssid;
    const char *password;
    const char *country_code;
    const char *bssid;
    bool static_enabled;
    const char *static_ip;
    const char *static_netmask;
    const char *static_gateway;
    const char *static_dns;
    bool wait_for_ip;
    int connect_timeout_ms;
    int max_retries;
} wifi_mgr_config_t;

typedef struct {
    char ssid[APP_WIFI_SSID_MAX_LEN];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
    uint8_t bssid[6];
} wifi_mgr_sta_ap_info_t;

typedef struct {
    uint32_t connect_count;          /* successful STA joins (DHCP complete) */
    uint32_t disconnect_count;       /* STA disconnect events */
    uint16_t reconnect_count;        /* scheduled reconnect attempts */
    uint16_t hard_recover_count;     /* forced driver-level recoveries */
    uint8_t last_disconnect_reason;  /* wifi_err_reason_t of the last drop */
    int64_t last_connect_uptime_ms;  /* uptime when the last join happened */
    int64_t last_session_ms;         /* duration of the previous session */
} wifi_mgr_link_stats_t;

esp_err_t wifi_mgr_init(const wifi_mgr_config_t *cfg);
bool wifi_mgr_is_connected(void);
esp_err_t wifi_mgr_force_reconnect(void);
esp_err_t wifi_mgr_force_transport_recover(void);
esp_err_t wifi_mgr_start_setup_ap(const wifi_mgr_ap_config_t *cfg);
esp_err_t wifi_mgr_stop_setup_ap(void);
bool wifi_mgr_is_setup_ap_active(void);
const char *wifi_mgr_get_setup_ap_ssid(void);
esp_err_t wifi_mgr_get_sta_ip(char *out, size_t out_len);
esp_err_t wifi_mgr_get_sta_gateway(char *out, size_t out_len);
esp_err_t wifi_mgr_get_ap_ip(char *out, size_t out_len);
esp_err_t wifi_mgr_get_sta_ap_info(wifi_mgr_sta_ap_info_t *out_info);
esp_err_t wifi_mgr_get_sta_rssi(int8_t *out_rssi_dbm);
esp_err_t wifi_mgr_get_link_stats(wifi_mgr_link_stats_t *out_stats);
esp_err_t wifi_mgr_scan(wifi_mgr_scan_result_t *results, size_t max_results, size_t *out_count);
