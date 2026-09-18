/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "diag/boot_guard.h"
#include "diag/crash_core.h"
#include "diag/display_flash_watch.h"
#include "diag/dsi_underrun_watch.h"
#include "diag/storage_guard.h"
#include "diag/system_log.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ha/ha_client.h"
#include "lwip/sockets.h"
#include "mqtt/panel_mqtt.h"
#include "net/net_health.h"
#include "net/wifi_mgr.h"
#include "sd/sd_card.h"
#include "ui/ui_lvgl_mem.h"
#include "ui/ui_screen_saver.h"

/* The temperature sensor is installed and enabled lazily on the first request
 * and stays enabled afterwards: install/enable cycle costs several register
 * syncs and the wall panel has no power budget concern. */
static temperature_sensor_handle_t s_temp_sensor = NULL;
static SemaphoreHandle_t s_temp_sensor_mutex = NULL;
static bool s_temp_sensor_failed = false;

static const char *chip_model_str(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32: return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    case CHIP_ESP32P4: return "ESP32-P4";
    default: return "unknown";
    }
}

/* Internal DRAM is split into pools with different capability masks (the plain
 * 8-bit pool, the 32-bit-capable pool and the DMA-capable reserve taken by
 * SPIRAM_MALLOC_RESERVE_INTERNAL). A single MALLOC_CAP_INTERNAL figure hides
 * which pool is actually tight, so the panel reports each of them. */
static void diagnostics_add_heap_region(cJSON *parent, const char *name, uint32_t caps)
{
    multi_heap_info_t info;
    memset(&info, 0, sizeof(info));
    heap_caps_get_info(&info, caps);

    size_t total = info.total_free_bytes + info.total_allocated_bytes;
    cJSON *obj = cJSON_AddObjectToObject(parent, name);
    if (obj == NULL) {
        return;
    }
    cJSON_AddNumberToObject(obj, "total", (double)total);
    cJSON_AddNumberToObject(obj, "free", (double)info.total_free_bytes);
    cJSON_AddNumberToObject(obj, "largest_block", (double)info.largest_free_block);
    cJSON_AddNumberToObject(obj, "free_min", (double)info.minimum_free_bytes);
    cJSON_AddNumberToObject(obj, "allocated", (double)info.total_allocated_bytes);
    cJSON_AddNumberToObject(obj, "alloc_blocks", (double)info.allocated_blocks);
    cJSON_AddNumberToObject(obj, "free_blocks", (double)info.free_blocks);
    cJSON_AddNumberToObject(
        obj, "fragmentation_pct",
        (info.total_free_bytes > 0)
            ? (double)(100 - (int)((info.largest_free_block * 100) / info.total_free_bytes))
            : 0.0);
}

static bool diagnostics_read_cpu_temp(float *out_celsius)
{
    if (out_celsius == NULL || s_temp_sensor_failed) {
        return false;
    }
    if (s_temp_sensor_mutex == NULL) {
        s_temp_sensor_mutex = xSemaphoreCreateMutex();
        if (s_temp_sensor_mutex == NULL) {
            s_temp_sensor_failed = true;
            return false;
        }
    }
    if (xSemaphoreTake(s_temp_sensor_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }

    bool ok = false;
    if (s_temp_sensor == NULL) {
        const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_temp_sensor) != ESP_OK) {
            s_temp_sensor = NULL;
            s_temp_sensor_failed = true;
        } else if (temperature_sensor_enable(s_temp_sensor) != ESP_OK) {
            (void)temperature_sensor_uninstall(s_temp_sensor);
            s_temp_sensor = NULL;
            s_temp_sensor_failed = true;
        }
    }

    float celsius = 0.0f;
    if (s_temp_sensor != NULL && temperature_sensor_get_celsius(s_temp_sensor, &celsius) == ESP_OK) {
        *out_celsius = celsius;
        ok = true;
    }

    xSemaphoreGive(s_temp_sensor_mutex);
    return ok;
}

static void add_wifi_section(cJSON *root)
{
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    if (wifi == NULL) {
        return;
    }

    bool connected = wifi_mgr_is_connected();
    cJSON_AddBoolToObject(wifi, "connected", connected);
    cJSON_AddBoolToObject(wifi, "setup_ap_active", wifi_mgr_is_setup_ap_active());

    char ip[48] = {0};
    if (wifi_mgr_get_sta_ip(ip, sizeof(ip)) == ESP_OK && ip[0] != '\0') {
        cJSON_AddStringToObject(wifi, "ip", ip);
    }

    wifi_mgr_sta_ap_info_t ap_info = {0};
    if (wifi_mgr_get_sta_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddStringToObject(wifi, "ssid", ap_info.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", ap_info.rssi);
        cJSON_AddNumberToObject(wifi, "channel", ap_info.channel);
        cJSON_AddNumberToObject(wifi, "authmode", ap_info.authmode);
    }

    const char *setup_ssid = wifi_mgr_get_setup_ap_ssid();
    if (setup_ssid != NULL && setup_ssid[0] != '\0') {
        cJSON_AddStringToObject(wifi, "setup_ap_ssid", setup_ssid);
    }

    wifi_mgr_link_stats_t stats;
    if (wifi_mgr_get_link_stats(&stats) == ESP_OK) {
        cJSON_AddNumberToObject(wifi, "connect_count", (double)stats.connect_count);
        cJSON_AddNumberToObject(wifi, "disconnect_count", (double)stats.disconnect_count);
        cJSON_AddNumberToObject(wifi, "reconnect_count", (double)stats.reconnect_count);
        cJSON_AddNumberToObject(wifi, "hard_recover_count", (double)stats.hard_recover_count);
        cJSON_AddNumberToObject(wifi, "last_disconnect_reason", (double)stats.last_disconnect_reason);
        cJSON_AddNumberToObject(wifi, "last_connect_uptime_ms", (double)stats.last_connect_uptime_ms);
        cJSON_AddNumberToObject(wifi, "last_session_ms", (double)stats.last_session_ms);
    }
}

/* The network watchdog escalates on its own; these counters show whether it has
 * been firing (a non-zero streak at the moment of a hang is the tell-tale). */
static void add_net_health_section(cJSON *root)
{
    net_health_stats_t stats;
    net_health_get_stats(&stats);

    cJSON *net = cJSON_AddObjectToObject(root, "net_health");
    if (net == NULL) {
        return;
    }

    cJSON_AddNumberToObject(net, "probe_rounds", (double)stats.probe_rounds);
    cJSON_AddNumberToObject(net, "ok_rounds", (double)stats.ok_rounds);
    cJSON_AddNumberToObject(net, "fail_rounds", (double)stats.fail_rounds);
    cJSON_AddNumberToObject(net, "fail_streak", (double)stats.fail_streak);
    cJSON_AddNumberToObject(net, "max_fail_streak", (double)stats.max_fail_streak);
    cJSON_AddNumberToObject(net, "reconnect_count", (double)stats.reconnect_count);
    cJSON_AddNumberToObject(net, "recover_count", (double)stats.recover_count);
    cJSON_AddNumberToObject(net, "restart_count", (double)stats.restart_count);
    cJSON_AddNumberToObject(net, "last_probe_ms", (double)stats.last_probe_ms);
    if (stats.last_ok_uptime_ms >= 0) {
        cJSON_AddNumberToObject(net, "last_ok_uptime_ms", (double)stats.last_ok_uptime_ms);
    }
    if (stats.gateway[0] != '\0') {
        cJSON_AddStringToObject(net, "gateway", stats.gateway);
    }
    if (stats.ha_host[0] != '\0') {
        cJSON_AddStringToObject(net, "ha_host", stats.ha_host);
        cJSON_AddNumberToObject(net, "ha_port", (double)stats.ha_port);
    }
}

/* Optional ?net_probe=<host>[:<port>] runs one probe round on demand and reports
 * what the panel can reach right now.  This is how the watchdog is verified
 * without cutting the panel's network: a reachable target proves the probes
 * still work, an unreachable one proves a failure would really be detected.
 * The value may be a hostname; "anchors" (or "1") probes the configured
 * gateway/Home Assistant only.  Runs synchronously, so allow a few seconds. */
static void add_net_probe_section(cJSON *root, httpd_req_t *req)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 256) {
        return;
    }

    char *qbuf = malloc(qlen + 1U);
    if (qbuf == NULL) {
        return;
    }
    if (httpd_req_get_url_query_str(req, qbuf, qlen + 1U) != ESP_OK) {
        free(qbuf);
        return;
    }

    char value[96] = {0};
    esp_err_t err = httpd_query_key_value(qbuf, "net_probe", value, sizeof(value));
    free(qbuf);
    if (err != ESP_OK) {
        return;
    }

    char *host = value;
    uint16_t port = 0;
    if (strcmp(value, "1") == 0 || strcmp(value, "anchors") == 0) {
        host = NULL;
    } else {
        char *colon = strrchr(value, ':');
        if (colon != NULL) {
            *colon = '\0';
            port = (uint16_t)strtoul(colon + 1, NULL, 10);
        }
        if (host[0] == '\0') {
            host = NULL;
        }
    }

    net_health_probe_result_t probe;
    net_health_probe_once(host, port, &probe);

    cJSON *json = cJSON_AddObjectToObject(root, "net_probe");
    if (json == NULL) {
        return;
    }
    if (probe.host[0] != '\0') {
        cJSON_AddStringToObject(json, "target", probe.host);
        cJSON_AddNumberToObject(json, "port", (double)probe.port);
        cJSON_AddBoolToObject(json, "target_ping_ok", probe.ping_ok);
        cJSON_AddBoolToObject(json, "target_tcp_ok", probe.tcp_ok);
    }
    cJSON_AddBoolToObject(json, "gateway_ping_ok", probe.gateway_ping_ok);
    cJSON_AddBoolToObject(json, "ha_tcp_ok", probe.ha_tcp_ok);
    cJSON_AddNumberToObject(json, "elapsed_ms", (double)probe.elapsed_ms);
}

static void add_ha_section(cJSON *root)
{
    cJSON *ha = cJSON_AddObjectToObject(root, "ha");
    if (ha == NULL) {
        return;
    }

    cJSON_AddBoolToObject(ha, "connected", ha_client_is_connected());
    cJSON_AddBoolToObject(ha, "initial_sync_done", ha_client_is_initial_sync_done());
    cJSON_AddBoolToObject(ha, "heavy_gate_busy", ha_client_heavy_gate_is_busy());

    ha_client_http_ctx_t ctx;
    if (ha_client_get_http_context(&ctx)) {
        /* Intentionally omits bearer_token: diagnostics must never leak it. */
        cJSON_AddStringToObject(ha, "base_url", ctx.base_url);
        cJSON_AddStringToObject(ha, "cert_common_name", ctx.cert_common_name);
    }

    ha_client_diagnostics_t diag;
    ha_client_get_diagnostics(&diag);
    cJSON_AddNumberToObject(ha, "missing_entities", (double)diag.total);

    ha_client_link_stats_t stats;
    ha_client_get_link_stats(&stats);

    cJSON *link = cJSON_AddObjectToObject(ha, "link");
    if (link != NULL) {
        cJSON_AddNumberToObject(link, "connect_count", (double)stats.connect_count);
        cJSON_AddNumberToObject(link, "disconnect_count", (double)stats.disconnect_count);
        cJSON_AddNumberToObject(link, "recover_count", (double)stats.recover_count);
        cJSON_AddNumberToObject(link, "error_streak", (double)stats.error_streak);
        cJSON_AddNumberToObject(link, "short_session_strikes", (double)stats.short_session_strikes);
        cJSON_AddNumberToObject(link, "last_connected_uptime_ms", (double)stats.last_connected_unix_ms);
        cJSON_AddNumberToObject(link, "last_session_ms", (double)stats.last_session_ms);
        cJSON_AddBoolToObject(link, "session_healthy",
            stats.connect_count > 0 && stats.error_streak == 0);
    }
}

static void add_mqtt_section(cJSON *root)
{
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    if (mqtt == NULL) {
        return;
    }

    const char *uri = panel_mqtt_broker_uri();
    bool has_uri = (uri != NULL && uri[0] != '\0');
    cJSON_AddBoolToObject(mqtt, "connected", panel_mqtt_is_connected());
    cJSON_AddBoolToObject(mqtt, "enabled", has_uri);
    if (has_uri) {
        cJSON_AddStringToObject(mqtt, "broker_uri", uri);
        cJSON_AddBoolToObject(mqtt, "tls", (strncmp(uri, "mqtts://", 8) == 0));
    }
}

static void add_ota_section(cJSON *root)
{
    cJSON *ota = cJSON_AddObjectToObject(root, "ota");
    if (ota == NULL) {
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    cJSON_AddStringToObject(ota, "running_partition", (running != NULL) ? running->label : "");
    cJSON_AddStringToObject(ota, "next_update_partition", (next != NULL) ? next->label : "");
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    cJSON_AddBoolToObject(ota, "rollback_enabled", true);
#else
    cJSON_AddBoolToObject(ota, "rollback_enabled", false);
#endif

    boot_guard_info_t boot;
    boot_guard_get_info(&boot);
    cJSON_AddNumberToObject(ota, "boot_count", (double)boot.boot_count);
    cJSON_AddStringToObject(ota, "reset_reason", boot_guard_reset_reason_str(boot.reset_reason));
    cJSON_AddNumberToObject(ota, "reset_reason_code", (double)boot.reset_reason);
    cJSON_AddBoolToObject(ota, "rollback_pending", boot.rollback_pending);
    cJSON_AddBoolToObject(ota, "boot_confirmed", boot.confirmed);
    cJSON_AddStringToObject(ota, "image_state", boot_guard_ota_state_str(boot.ota_state));
}

/* Crash forensics: the panic that rebooted the panel last time is decoded from
 * the core dump partition at boot; surface it through /api/diagnostics so a
 * single poll is enough to see whether the unit is in a reboot loop. */
static void add_crash_section(cJSON *root)
{
    const crash_core_info_t *crash = crash_core_get_info();

    cJSON *obj = cJSON_AddObjectToObject(root, "crash");
    if (obj == NULL) {
        return;
    }

    cJSON_AddBoolToObject(obj, "dump_present", crash->present);
    cJSON_AddBoolToObject(obj, "dump_partition", crash->partition_ok);
    cJSON_AddBoolToObject(obj, "dump_from_same_image", crash->app_match);
    cJSON_AddNumberToObject(obj, "dump_size", (double)crash->size);
    if (crash->check_err != ESP_OK) {
        cJSON_AddNumberToObject(obj, "dump_check_error", (double)crash->check_err);
    }
    if (!crash->present) {
        return;
    }

    cJSON_AddStringToObject(obj, "task", crash->task);
    cJSON_AddStringToObject(obj, "reason", crash->reason);
    cJSON_AddStringToObject(obj, "cause", crash->cause_str);
    cJSON_AddNumberToObject(obj, "cause_code", (double)crash->cause);
    cJSON_AddNumberToObject(obj, "pc", (double)crash->pc);
    cJSON_AddNumberToObject(obj, "fault_addr", (double)crash->fault_addr);
    cJSON_AddNumberToObject(obj, "ra", (double)crash->ra);
    cJSON_AddNumberToObject(obj, "sp", (double)crash->sp);
    cJSON_AddStringToObject(obj, "app_sha_prefix", crash->app_sha);
}

/* Screen changes seen by the flash detector, including the ones that lasted less
 * than one fine-pass interval: the counters alone cannot separate a sub-frame
 * flash from a glitch, so the newest coarse-pass events travel with them.  The
 * section is absent on panel variants that have no such detector. */
static void add_flash_section(cJSON *root)
{
    char summary[256];
    display_flash_watch_summary(summary, sizeof(summary));

    /* Static, like the other multi-hundred-byte dumps here: esp_http_server
     * serves one request at a time from a single task, and the HTTP task stack is
     * not the place for a kilobyte buffer. */
    static char fast[DISPLAY_FLASH_WATCH_FAST_TEXT_LEN];
    display_flash_watch_get_fast(fast, sizeof(fast));

    if (summary[0] == '\0' && fast[0] == '\0') {
        return;
    }

    cJSON *obj = cJSON_AddObjectToObject(root, "flash_watch");
    if (obj == NULL) {
        return;
    }
    cJSON_AddBoolToObject(obj, "enabled", display_flash_watch_get_enabled());
    if (summary[0] != '\0') {
        cJSON_AddStringToObject(obj, "counters", summary);
    }
    if (fast[0] != '\0') {
        cJSON_AddStringToObject(obj, "fast_history", fast);
    }
}

/* The DSI half of the same hunt: bridge FIFO bursts, D-PHY lock losses and any
 * host link error bit ever latched, plus what the mitigation registers read
 * back as right now. */
static void add_dsi_section(cJSON *root)
{
    static char summary[768];
    dsi_underrun_watch_summary(summary, sizeof(summary));
    if (summary[0] == '\0') {
        return;
    }

    cJSON *obj = cJSON_AddObjectToObject(root, "dsi_watch");
    if (obj == NULL) {
        return;
    }
    cJSON_AddStringToObject(obj, "counters", summary);
    cJSON_AddNumberToObject(obj, "underruns", (double)dsi_underrun_watch_count());
    cJSON_AddNumberToObject(obj, "bursts", (double)dsi_underrun_watch_bursts());
    cJSON_AddNumberToObject(obj, "fifo_min", (double)dsi_underrun_watch_fifo_min());
    cJSON_AddNumberToObject(obj, "fifo_zero_samples", (double)dsi_underrun_watch_fifo_zero());
    cJSON_AddNumberToObject(obj, "fifo_dry_episodes", (double)dsi_underrun_watch_fifo_zero_runs());
    cJSON_AddNumberToObject(obj, "host_pixel_fifo_dry", (double)dsi_underrun_watch_host_pld_empty());
    cJSON_AddNumberToObject(obj, "host_pixel_fifo_dry_max_ms",
                            (double)dsi_underrun_watch_host_pld_max_ms());
    cJSON_AddNumberToObject(obj, "host_pixel_fifo_dry_long",
                            (double)dsi_underrun_watch_host_pld_long_runs());
    cJSON_AddNumberToObject(obj, "host_status_edges",
                            (double)dsi_underrun_watch_host_status_edges());
    cJSON_AddNumberToObject(obj, "phy_unlocks", (double)dsi_underrun_watch_phy_unlocks());
    cJSON_AddNumberToObject(obj, "mitigation_clobbers", (double)dsi_underrun_watch_clobbers());
    const uint32_t since = dsi_underrun_watch_since_last_ms();
    if (since != UINT32_MAX) {
        cJSON_AddNumberToObject(obj, "since_last_ms", (double)since);
    }
}

/* The frame-gap witness plus the flash-time meter behind it: `gaps` counts the
 * times the pixel feed was stalled long enough for the bridge filler to reach
 * the glass, and `flash_ops` says which internal-flash writer owned the flash
 * when it happened.  A healthy panel shows frames climbing at the frame rate
 * with gaps=0. */
static void add_screen_section(cJSON *root)
{
    dsi_frame_gap_stats_t gap = {0};
    dsi_frame_gap_watch_stats(&gap);

    cJSON *obj = cJSON_AddObjectToObject(root, "screen_watch");
    if (obj == NULL) {
        return;
    }
    cJSON_AddBoolToObject(obj, "armed", gap.armed);
    cJSON_AddNumberToObject(obj, "frames", (double)gap.frames);
    cJSON_AddNumberToObject(obj, "gaps", (double)gap.gaps);
    cJSON_AddNumberToObject(obj, "gap_max_ms", (double)gap.max_ms);
    cJSON_AddNumberToObject(obj, "gap_last_ms", (double)gap.last_ms);
    cJSON_AddNumberToObject(obj, "frame_interval_us_max", (double)gap.max_interval_us);
    cJSON_AddNumberToObject(obj, "gaps_with_flash", (double)gap.with_flash);
    cJSON_AddNumberToObject(obj, "gaps_lost", (double)gap.lost);

    cJSON_AddBoolToObject(obj, "log_on_sd", sd_card_is_mounted());

    /* A reveal that a wake dismissed before the light wallpaper had faded in is
     * the blink the user reports as a light-blue screen flash; the counter is
     * the only witness fast enough to catch it. */
    uint32_t reveals = 0, blips = 0;
    int64_t last_ms = -1;
    ui_screen_saver_get_reveal_stats(&reveals, &blips, &last_ms);
    cJSON *saver = cJSON_AddObjectToObject(obj, "saver");
    if (saver != NULL) {
        cJSON_AddNumberToObject(saver, "reveals", (double)reveals);
        cJSON_AddNumberToObject(saver, "reveal_blips", (double)blips);
        cJSON_AddNumberToObject(saver, "reveal_last_ms", (double)last_ms);
    }

    cJSON *ops = cJSON_AddObjectToObject(obj, "flash_ops");
    if (ops == NULL) {
        return;
    }
    for (uint32_t kind = 0; kind < (uint32_t)FLASH_OP_KIND_COUNT; kind++) {
        const char *name = storage_guard_flash_op_kind_name(kind);
        if (name == NULL) {
            continue;
        }
        cJSON *entry = cJSON_AddObjectToObject(ops, name);
        if (entry == NULL) {
            continue;
        }
        cJSON_AddNumberToObject(entry, "count", (double)storage_guard_flash_op_count(kind));
        cJSON_AddNumberToObject(entry, "total_ms", (double)storage_guard_flash_op_total_ms(kind));
        cJSON_AddNumberToObject(entry, "max_ms", (double)storage_guard_flash_op_max_ms(kind));
        cJSON_AddNumberToObject(entry, "slow", (double)storage_guard_flash_op_slow(kind));
    }
}

esp_err_t api_diagnostics_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *app = cJSON_AddObjectToObject(root, "app");
    if (app != NULL) {
        cJSON_AddStringToObject(app, "name", APP_NAME);
        cJSON_AddStringToObject(app, "version",
            (desc != NULL && desc->version[0] != '\0') ? desc->version : "unknown");
        cJSON_AddStringToObject(app, "project",
            (desc != NULL && desc->project_name[0] != '\0') ? desc->project_name : APP_NAME);
        cJSON_AddStringToObject(app, "idf_version", esp_get_idf_version());
        cJSON_AddStringToObject(app, "build_date",
            (desc != NULL) ? desc->date : "");
        cJSON_AddStringToObject(app, "build_time",
            (desc != NULL) ? desc->time : "");
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *hw = cJSON_AddObjectToObject(root, "chip");
    if (hw != NULL) {
        cJSON_AddStringToObject(hw, "model", chip_model_str(chip.model));
        cJSON_AddNumberToObject(hw, "cores", chip.cores);
        cJSON_AddNumberToObject(hw, "revision", chip.revision);
        cJSON_AddNumberToObject(hw, "features", (double)chip.features);
        cJSON_AddNumberToObject(hw, "screen_w", APP_SCREEN_WIDTH);
        cJSON_AddNumberToObject(hw, "screen_h", APP_SCREEN_HEIGHT);
    }

    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    cJSON *mem = cJSON_AddObjectToObject(root, "memory");
    if (mem != NULL) {
        cJSON_AddNumberToObject(mem, "heap_free", (double)heap_free);
        cJSON_AddNumberToObject(mem, "heap_free_min", (double)heap_min);
        cJSON_AddNumberToObject(mem, "heap_largest_block", (double)heap_largest);
        cJSON_AddNumberToObject(mem, "heap_fragmentation_pct",
            (heap_free > 0) ? (double)(100 - (int)((heap_largest * 100) / heap_free)) : 0.0);
        cJSON_AddNumberToObject(mem, "psram_free", (double)psram_free);
        cJSON_AddNumberToObject(mem, "psram_largest_block", (double)psram_largest);
        cJSON_AddNumberToObject(mem, "psram_fragmentation_pct",
            (psram_free > 0) ? (double)(100 - (int)((psram_largest * 100) / psram_free)) : 0.0);

        cJSON_AddNumberToObject(mem, "iram_free", (double)heap_caps_get_free_size(MALLOC_CAP_IRAM_8BIT));
        cJSON_AddNumberToObject(mem, "iram_largest_block", (double)heap_caps_get_largest_free_block(MALLOC_CAP_IRAM_8BIT));

        cJSON *regions = cJSON_AddObjectToObject(mem, "regions");
        if (regions != NULL) {
            diagnostics_add_heap_region(regions, "internal", MALLOC_CAP_INTERNAL);
            diagnostics_add_heap_region(regions, "internal_dma", MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            diagnostics_add_heap_region(regions, "internal_32bit", MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
            diagnostics_add_heap_region(regions, "psram", MALLOC_CAP_SPIRAM);
            diagnostics_add_heap_region(regions, "default", MALLOC_CAP_DEFAULT);
        }
    }

    ui_lvgl_mem_stats_t lvgl_mem;
    ui_lvgl_mem_get_stats(&lvgl_mem);
    cJSON *lvgl = cJSON_AddObjectToObject(root, "lvgl");
    if (lvgl != NULL) {
        cJSON_AddNumberToObject(lvgl, "used_bytes", (double)lvgl_mem.used_bytes);
        cJSON_AddNumberToObject(lvgl, "peak_bytes", (double)lvgl_mem.peak_bytes);
        cJSON_AddNumberToObject(lvgl, "live_blocks", (double)lvgl_mem.block_count);
        cJSON_AddNumberToObject(lvgl, "alloc_count", (double)lvgl_mem.alloc_count);
        cJSON_AddNumberToObject(lvgl, "free_count", (double)lvgl_mem.free_count);
        cJSON_AddNumberToObject(lvgl, "fail_count", (double)lvgl_mem.fail_count);
    }

    float cpu_temp = 0.0f;
    if (diagnostics_read_cpu_temp(&cpu_temp)) {
        cJSON_AddNumberToObject(root, "cpu_temp_c", (double)cpu_temp);
    }

    add_wifi_section(root);
    add_net_health_section(root);
    add_net_probe_section(root, req);
    add_ha_section(root);
    add_mqtt_section(root);
    add_ota_section(root);
    add_crash_section(root);
    add_flash_section(root);
    add_dsi_section(root);
    add_screen_section(root);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

/* Client address of the request, as a dotted IPv4 string ("-" when unknown).
 *
 * The HTTP server listens on a dual-stack socket (PF_INET6) whenever IPv6 is
 * enabled, so even a plain IPv4 browser arrives here as AF_INET6 with a
 * v4-mapped address (::ffff:a.b.c.d) - reporting "unknown" for those would
 * defeat the whole point of logging the caller. */
static void diagnostics_peer_addr(httpd_req_t *req, char *out, size_t out_len)
{
    snprintf(out, out_len, "-");
    if (req == NULL) {
        return;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return;
    }

    struct sockaddr_storage addr = {0};
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) != 0) {
        return;
    }

    if (addr.ss_family == AF_INET && addr_len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)&addr;
        const uint8_t *b = (const uint8_t *)&sa->sin_addr.s_addr;
        snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3]);
        return;
    }

    if (addr.ss_family == AF_INET6 && addr_len >= sizeof(struct sockaddr_in6)) {
        const uint8_t *b = (const uint8_t *)&((const struct sockaddr_in6 *)&addr)->sin6_addr;
        const bool mapped = (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12) == 0);
        if (mapped) {
            snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)b[12], (unsigned)b[13], (unsigned)b[14], (unsigned)b[15]);
        } else if (inet_ntop(AF_INET6, b, out, (socklen_t)out_len) == NULL) {
            snprintf(out, out_len, "-");
        }
    }
}

/* Keep the log useful: report each distinct caller of the compact status
 * endpoint once, so an unknown LAN monitor can be identified, but never spam a
 * line per poll (that endpoint is meant to be polled every few seconds). */
static void diagnostics_note_status_caller(const char *addr)
{
    static char seen[4][20];
    static size_t seen_count;

    if (addr == NULL || addr[0] == '\0' || strcmp(addr, "-") == 0) {
        return;
    }
    for (size_t i = 0; i < seen_count; i++) {
        if (strcmp(seen[i], addr) == 0) {
            return;
        }
    }
    if (seen_count < (sizeof(seen) / sizeof(seen[0]))) {
        snprintf(seen[seen_count], sizeof(seen[0]), "%s", addr);
        seen_count++;
    }
    /* The persistent log only captures warnings and errors, so this one has to
     * go through the explicit info path to be visible after the fact. */
    system_log_write_info("api_status", "status polled by %s", addr);
}

/* Compact health summary: a handful of scalars that an external monitor (Home
 * Assistant REST sensor, uptime ping, network dashboard) can poll cheaply and
 * every few seconds, unlike the full /api/diagnostics payload.  Also answers
 * the "/api/status" path that some HA integrations guess at - leaving it
 * unregistered made the HTTP server log two warning lines per poll. */
esp_err_t api_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    const esp_app_desc_t *desc = esp_app_get_description();

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "name", APP_NAME);
    cJSON_AddStringToObject(root, "version",
                            (desc != NULL && desc->version[0] != '\0') ? desc->version : "unknown");
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    cJSON_AddBoolToObject(root, "wifi_connected", wifi_mgr_is_connected());
    char ip[48] = {0};
    if (wifi_mgr_get_sta_ip(ip, sizeof(ip)) == ESP_OK && ip[0] != '\0') {
        cJSON_AddStringToObject(root, "ip", ip);
    }
    wifi_mgr_sta_ap_info_t ap_info = {0};
    if (wifi_mgr_get_sta_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
    }

    cJSON_AddBoolToObject(root, "ha_connected", ha_client_is_connected());
    cJSON_AddBoolToObject(root, "mqtt_connected", panel_mqtt_is_connected());
    cJSON_AddBoolToObject(root, "sd_mounted", sd_card_is_mounted());
    cJSON_AddBoolToObject(root, "storage_busy", storage_guard_active());

    add_ota_section(root);

    char peer[20];
    diagnostics_peer_addr(req, peer, sizeof(peer));
    diagnostics_note_status_caller(peer);
    cJSON_AddStringToObject(root, "client", peer);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

/* ---------------------------------------------------------------------- */
/* Core dump endpoints                                                     */
/*                                                                         */
/* The stored dump is an ELF core file and is only useful together with the */
/* matching .elf, so /api/crash exposes a compact human-readable summary    */
/* while /api/crash/raw hands the file over for `idf.py coredump-info`.     */
/* ---------------------------------------------------------------------- */

esp_err_t api_crash_get_handler(httpd_req_t *req)
{
    const crash_core_info_t *crash = crash_core_get_info();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddBoolToObject(root, "present", crash->present);
    cJSON_AddBoolToObject(root, "partition", crash->partition_ok);
    cJSON_AddBoolToObject(root, "same_image", crash->app_match);
    cJSON_AddNumberToObject(root, "size", (double)crash->size);
    cJSON_AddStringToObject(root, "source", "core dump written by the panic handler of a previous boot");

    char peer[20];
    diagnostics_peer_addr(req, peer, sizeof(peer));
    cJSON_AddStringToObject(root, "client", peer);

    if (crash->present) {
        cJSON_AddStringToObject(root, "task", crash->task);
        cJSON_AddStringToObject(root, "reason", crash->reason);
        cJSON_AddStringToObject(root, "cause", crash->cause_str);
        cJSON_AddNumberToObject(root, "cause_code", (double)crash->cause);
        cJSON_AddNumberToObject(root, "pc", (double)crash->pc);
        cJSON_AddNumberToObject(root, "fault_addr", (double)crash->fault_addr);
        cJSON_AddNumberToObject(root, "ra", (double)crash->ra);
        cJSON_AddNumberToObject(root, "sp", (double)crash->sp);
        cJSON_AddStringToObject(root, "app_sha_prefix", crash->app_sha);
        cJSON_AddStringToObject(root, "raw", "/api/crash/raw");
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);

    ESP_LOGW("crashapi", "core dump summary requested by %s (present=%d size=%u)", peer,
             (int)crash->present, (unsigned)crash->size);
    return err;
}

esp_err_t api_crash_raw_get_handler(httpd_req_t *req)
{
    const crash_core_info_t *crash = crash_core_get_info();
    if (!crash->present || crash->size == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"no core dump stored\"}");
    }

    /* Chunked transfer: the dump can be hundreds of kilobytes and there is no
     * contiguous internal buffer that large - which is also why the ring log
     * itself cannot hold the raw dump. */
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.elf\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    enum { CHUNK = 4096 };
    char *buf = malloc(CHUNK);
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }

    esp_err_t err = ESP_OK;
    size_t offset = 0;
    while (offset < crash->size) {
        size_t want = crash->size - offset;
        if (want > CHUNK) {
            want = CHUNK;
        }

        size_t got = 0;
        err = crash_core_read(offset, buf, want, &got);
        if (err != ESP_OK || got == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, buf, got) != ESP_OK) {
            err = ESP_FAIL; /* client closed the connection */
            break;
        }
        offset += got;

        /* Flashing the dump is a long flash read loop competing with the LVGL
         * task for the cache; yield a tick per chunk so the UI keeps drawing. */
        vTaskDelay(1);
    }

    free(buf);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGW("crashapi", "core dump (%u bytes) downloaded", (unsigned)crash->size);
    } else {
        ESP_LOGE("crashapi", "core dump download aborted at %u/%u bytes: %s", (unsigned)offset,
                 (unsigned)crash->size, esp_err_to_name(err));
    }
    return err;
}

esp_err_t api_crash_erase_post_handler(httpd_req_t *req)
{
    esp_err_t erase_err = crash_core_erase();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", erase_err == ESP_OK);
    cJSON_AddStringToObject(root, "result", esp_err_to_name(erase_err));

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}
