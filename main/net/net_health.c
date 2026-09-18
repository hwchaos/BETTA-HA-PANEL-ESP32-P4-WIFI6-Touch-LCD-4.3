/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "net/net_health.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "apps/ping/ping_sock.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "diag/system_log.h"
#include "net/wifi_mgr.h"
#include "util/log_tags.h"

#define NET_HEALTH_ROUND_INTERVAL_MS   30000U
#define NET_HEALTH_FIRST_ROUND_MS      45000U
#define NET_HEALTH_PING_COUNT          2U
#define NET_HEALTH_PING_TIMEOUT_MS     1200U
#define NET_HEALTH_TCP_TIMEOUT_MS      2000
#define NET_HEALTH_HA_DEFAULT_PORT     8123U
#define NET_HEALTH_MIN_UPTIME_FOR_RESTART_MS 600000
#define NET_HEALTH_SETTLE_RECONNECT_MS 10000U
#define NET_HEALTH_SETTLE_RECOVER_MS   20000U

#define NET_HEALTH_STREAK_RECONNECT    3U
#define NET_HEALTH_STREAK_RECOVER      6U
#define NET_HEALTH_STREAK_RESTART      10U

static net_health_stats_t s_stats;
static SemaphoreHandle_t s_ping_done = NULL;
static SemaphoreHandle_t s_probe_mutex = NULL;
static volatile uint32_t s_ping_replies = 0;
static bool s_started = false;

static void net_health_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    (void)args;
    s_ping_replies++;
}

static void net_health_ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl;
    SemaphoreHandle_t done = (SemaphoreHandle_t)args;
    if (done != NULL) {
        xSemaphoreGive(done);
    }
}

/* Accepts dotted-quad addresses as well as names (the on-demand probe is
 * typically pointed at a hostname the operator knows). */
static bool net_health_resolve_ipv4(const char *host, struct in_addr *out)
{
    if (host == NULL || host[0] == '\0' || out == NULL) {
        return false;
    }
    if (inet_pton(AF_INET, host, out) == 1) {
        return true;
    }

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
        return false;
    }

    bool resolved = false;
    for (const struct addrinfo *it = res; it != NULL && !resolved; it = it->ai_next) {
        if (it->ai_family == AF_INET && it->ai_addr != NULL) {
            *out = ((const struct sockaddr_in *)it->ai_addr)->sin_addr;
            resolved = true;
        }
    }
    freeaddrinfo(res);
    return resolved;
}

/* ICMP echo through esp_ping.  Returns true on at least one reply.  The ping
 * session is deleted by the caller (lwip must not free it from inside the
 * callback).  The reply counter and the completion semaphore are shared with
 * the on-demand probe, so the whole session is serialised. */
static bool net_health_ping_locked(const char *ip)
{
    if (ip == NULL || ip[0] == '\0' || s_ping_done == NULL) {
        return false;
    }

    struct in_addr addr4 = {0};
    if (!net_health_resolve_ipv4(ip, &addr4)) {
        return false;
    }

    ip_addr_t target = {0};
    inet_addr_to_ip4addr(ip_2_ip4(&target), &addr4);

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = NET_HEALTH_PING_COUNT;
    config.interval_ms = 400;
    config.timeout_ms = NET_HEALTH_PING_TIMEOUT_MS;
    config.target_addr = target;
    config.task_stack_size = 4096;

    esp_ping_callbacks_t cbs = {
        .cb_args = (void *)s_ping_done,
        .on_ping_success = net_health_ping_success,
        .on_ping_end = net_health_ping_end,
    };

    esp_ping_handle_t session = NULL;
    if (esp_ping_new_session(&config, &cbs, &session) != ESP_OK) {
        return false;
    }

    xSemaphoreTake(s_ping_done, 0);
    s_ping_replies = 0;

    bool alive = false;
    if (esp_ping_start(session) == ESP_OK) {
        const TickType_t wait = pdMS_TO_TICKS(
            (NET_HEALTH_PING_COUNT * (config.interval_ms + NET_HEALTH_PING_TIMEOUT_MS)) + 2000U);
        if (xSemaphoreTake(s_ping_done, wait) == pdTRUE) {
            alive = s_ping_replies > 0;
        } else {
            ESP_LOGD(TAG_NET_HEALTH, "ping %s: session did not finish in time", ip);
        }
    }

    esp_ping_stop(session);
    esp_ping_delete_session(session);
    return alive;
}

static bool net_health_ping(const char *ip)
{
    if (s_probe_mutex == NULL) {
        /* Without the lock two concurrent sessions would share one reply counter
         * and one completion semaphore, so refuse to probe rather than lie. */
        return false;
    }
    if (xSemaphoreTake(s_probe_mutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        return false;
    }
    const bool alive = net_health_ping_locked(ip);
    xSemaphoreGive(s_probe_mutex);
    return alive;
}

/* A TCP connect that finishes - even with a refusal or a reset - proves that
 * packets travel to the peer and back, which is exactly what the watchdog has
 * to distinguish from a dead C6 transport. */
static bool net_health_tcp_probe(const char *ip, uint16_t port)
{
    if (ip == NULL || ip[0] == '\0' || port == 0) {
        return false;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!net_health_resolve_ipv4(ip, &addr.sin_addr)) {
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return false;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    bool alive = false;
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        alive = true;
    } else {
        const int connect_errno = errno;
        if (connect_errno == EINPROGRESS || connect_errno == EALREADY) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            struct timeval tv = {
                .tv_sec = NET_HEALTH_TCP_TIMEOUT_MS / 1000,
                .tv_usec = (NET_HEALTH_TCP_TIMEOUT_MS % 1000) * 1000,
            };
            int sel = select(sock + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0) {
                int so_err = 0;
                socklen_t so_len = sizeof(so_err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &so_len) == 0) {
                    alive = (so_err == 0 || so_err == ECONNREFUSED || so_err == ECONNRESET);
                }
            }
        } else if (connect_errno == ECONNREFUSED || connect_errno == ECONNRESET) {
            alive = true;
        } else {
            ESP_LOGD(TAG_NET_HEALTH, "TCP %s:%u failed: errno %d", ip, (unsigned)port, connect_errno);
        }
    }

    close(sock);
    return alive;
}

static bool net_health_parse_endpoint(const char *url, char *host, size_t host_size, uint16_t *port)
{
    if (host == NULL || host_size == 0 || port == NULL) {
        return false;
    }
    host[0] = '\0';
    *port = NET_HEALTH_HA_DEFAULT_PORT;
    if (url == NULL || url[0] == '\0') {
        return false;
    }

    const char *authority = url;
    const char *scheme = strstr(url, "://");
    if (scheme != NULL) {
        authority = scheme + 3;
    }

    const char *slash = strchr(authority, '/');
    size_t authority_len = (slash != NULL) ? (size_t)(slash - authority) : strlen(authority);

    /* Drop any "user:password@" prefix and a trailing "/path". */
    const char *at = NULL;
    for (size_t i = 0; i < authority_len; i++) {
        if (authority[i] == '@') {
            at = &authority[i];
        }
    }
    if (at != NULL) {
        authority_len -= (size_t)(at + 1 - authority);
        authority = at + 1;
    }

    const char *last_colon = NULL;
    for (size_t i = 0; i < authority_len; i++) {
        if (authority[i] == ':') {
            last_colon = &authority[i];
        }
    }

    size_t host_len = authority_len;
    if (last_colon != NULL) {
        host_len = (size_t)(last_colon - authority);
        const int parsed = atoi(last_colon + 1);
        if (parsed > 0 && parsed <= 65535) {
            *port = (uint16_t)parsed;
        }
    }

    if (host_len == 0 || host_len >= host_size) {
        return false;
    }
    memcpy(host, authority, host_len);
    host[host_len] = '\0';
    return true;
}

static bool net_health_round_is_alive(void)
{
    if (s_stats.gateway[0] != '\0' && net_health_ping(s_stats.gateway)) {
        return true;
    }

    if (s_stats.gateway[0] != '\0' &&
        (net_health_tcp_probe(s_stats.gateway, 80) || net_health_tcp_probe(s_stats.gateway, 443))) {
        return true;
    }

    if (s_stats.ha_host[0] != '\0' && net_health_tcp_probe(s_stats.ha_host, s_stats.ha_port)) {
        return true;
    }

    return false;
}

static bool net_health_has_anchor(void)
{
    return s_stats.gateway[0] != '\0' || s_stats.ha_host[0] != '\0';
}

static void net_health_refresh_targets(void)
{
    char gateway[sizeof(s_stats.gateway)] = {0};
    if (wifi_mgr_get_sta_gateway(gateway, sizeof(gateway)) != ESP_OK || gateway[0] == '\0') {
        return;
    }
    if (strncmp(gateway, s_stats.gateway, sizeof(gateway)) != 0) {
        ESP_LOGI(TAG_NET_HEALTH, "watchdog anchor: gateway %s", gateway);
        memcpy(s_stats.gateway, gateway, sizeof(s_stats.gateway));
    }
}

static void net_health_escalate(uint8_t streak)
{
    if (streak == NET_HEALTH_STREAK_RECONNECT) {
        s_stats.reconnect_count++;
        ESP_LOGW(TAG_NET_HEALTH, "network dead for %u rounds: forcing Wi-Fi reconnect", (unsigned)streak);
        system_log_write(TAG_NET_HEALTH, "network dead for %u rounds: forcing Wi-Fi reconnect", (unsigned)streak);
        (void)wifi_mgr_force_reconnect();
        vTaskDelay(pdMS_TO_TICKS(NET_HEALTH_SETTLE_RECONNECT_MS));
        return;
    }

    if (streak == NET_HEALTH_STREAK_RECOVER) {
        s_stats.recover_count++;
        ESP_LOGW(TAG_NET_HEALTH, "network still dead after %u rounds: hard-recovering the hosted Wi-Fi link",
                 (unsigned)streak);
        system_log_write(TAG_NET_HEALTH, "network still dead after %u rounds: C6 host hard recover", (unsigned)streak);
        (void)wifi_mgr_force_transport_recover();
        vTaskDelay(pdMS_TO_TICKS(NET_HEALTH_SETTLE_RECOVER_MS));
        return;
    }

    /* >= and not ==: the guard below can push the streak past the threshold
     * (e.g. the network dies during the first minutes after a boot), and the
     * restart must still happen once the uptime requirement is met. */
    if (streak >= NET_HEALTH_STREAK_RESTART) {
        if ((esp_timer_get_time() / 1000) < NET_HEALTH_MIN_UPTIME_FOR_RESTART_MS) {
            if (streak == NET_HEALTH_STREAK_RESTART) {
                ESP_LOGW(TAG_NET_HEALTH, "network dead for %u rounds but uptime is too short to restart: waiting",
                         (unsigned)streak);
            }
            return;
        }
        s_stats.restart_count++;
        ESP_LOGE(TAG_NET_HEALTH, "network unreachable for %u rounds: restarting the panel", (unsigned)streak);
        system_log_write(TAG_NET_HEALTH, "network unreachable for %u rounds: restarting the panel", (unsigned)streak);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

static void net_health_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(NET_HEALTH_FIRST_ROUND_MS));

    for (;;) {
        if (!wifi_mgr_is_connected()) {
            /* The Wi-Fi manager owns this state: it got its disconnect event and
             * its own ladder is running, so the watchdog must stay out of it. */
            if (s_stats.fail_streak != 0) {
                ESP_LOGI(TAG_NET_HEALTH, "Wi-Fi link is down: watchdog idle");
            }
            s_stats.fail_streak = 0;
            vTaskDelay(pdMS_TO_TICKS(NET_HEALTH_ROUND_INTERVAL_MS));
            continue;
        }

        net_health_refresh_targets();

        if (!net_health_has_anchor()) {
            ESP_LOGD(TAG_NET_HEALTH, "no anchor yet (no gateway, no HA url): watchdog idle");
            s_stats.fail_streak = 0;
            vTaskDelay(pdMS_TO_TICKS(NET_HEALTH_ROUND_INTERVAL_MS));
            continue;
        }

        const int64_t started_us = esp_timer_get_time();
        const bool alive = net_health_round_is_alive();
        s_stats.last_probe_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
        s_stats.probe_rounds++;

        if (alive) {
            if (s_stats.fail_streak != 0) {
                ESP_LOGI(TAG_NET_HEALTH, "network reachable again after %u dead rounds", (unsigned)s_stats.fail_streak);
                system_log_write(TAG_NET_HEALTH, "network reachable again after %u dead rounds",
                                 (unsigned)s_stats.fail_streak);
            }
            s_stats.ok_rounds++;
            s_stats.fail_streak = 0;
            s_stats.last_ok_uptime_ms = esp_timer_get_time() / 1000;
        } else {
            s_stats.fail_rounds++;
            if (s_stats.fail_streak < 255U) {
                s_stats.fail_streak++;
            }
            if (s_stats.fail_streak > s_stats.max_fail_streak) {
                s_stats.max_fail_streak = s_stats.fail_streak;
            }
            ESP_LOGW(TAG_NET_HEALTH,
                     "no answer from the network (%u round(s) in a row, probe %u ms, gw=%s, ha=%s:%u)",
                     (unsigned)s_stats.fail_streak,
                     (unsigned)s_stats.last_probe_ms,
                     s_stats.gateway[0] != '\0' ? s_stats.gateway : "-",
                     s_stats.ha_host[0] != '\0' ? s_stats.ha_host : "-",
                     (unsigned)s_stats.ha_port);
            net_health_escalate(s_stats.fail_streak);
        }

        vTaskDelay(pdMS_TO_TICKS(NET_HEALTH_ROUND_INTERVAL_MS));
    }
}

esp_err_t net_health_start(const char *ha_ws_url)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_started = true;

    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.last_ok_uptime_ms = -1;

    s_ping_done = xSemaphoreCreateBinary();
    s_probe_mutex = xSemaphoreCreateMutex();
    if (s_ping_done == NULL || s_probe_mutex == NULL) {
        s_started = false;
        ESP_LOGE(TAG_NET_HEALTH, "cannot create the probe primitives, watchdog disabled");
        return ESP_FAIL;
    }

    char host[sizeof(s_stats.ha_host)] = {0};
    uint16_t port = 0;
    if (net_health_parse_endpoint(ha_ws_url, host, sizeof(host), &port)) {
        memcpy(s_stats.ha_host, host, sizeof(s_stats.ha_host));
        s_stats.ha_port = port;
        ESP_LOGI(TAG_NET_HEALTH, "watchdog anchor: Home Assistant at %s:%u", s_stats.ha_host, (unsigned)s_stats.ha_port);
    } else {
        ESP_LOGW(TAG_NET_HEALTH, "no usable Home Assistant URL, the gateway alone anchors the watchdog");
    }

    if (xTaskCreate(net_health_task, "net_health", 5120, NULL, 3, NULL) != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG_NET_HEALTH, "cannot create the watchdog task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void net_health_get_stats(net_health_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stats;
}

void net_health_probe_once(const char *host, uint16_t port, net_health_probe_result_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->port = port;

    const int64_t started_us = esp_timer_get_time();

    char gateway[20] = {0};
    if (s_stats.gateway[0] != '\0') {
        memcpy(gateway, s_stats.gateway, sizeof(gateway) - 1);
    } else if (wifi_mgr_get_sta_gateway(gateway, sizeof(gateway)) != ESP_OK) {
        gateway[0] = '\0';
    }
    out->gateway_ping_ok = (gateway[0] != '\0') && net_health_ping(gateway);

    if (s_stats.ha_host[0] != '\0') {
        out->ha_tcp_ok = net_health_tcp_probe(s_stats.ha_host, s_stats.ha_port);
    }

    if (host != NULL && host[0] != '\0') {
        strlcpy(out->host, host, sizeof(out->host));
        out->ping_ok = net_health_ping(out->host);
        out->tcp_ok = net_health_tcp_probe(out->host, (port != 0) ? port : NET_HEALTH_HA_DEFAULT_PORT);
    }

    out->elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
}
