/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/http_guard.h"

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "diag/system_log.h"
#include "util/log_tags.h"

#define HTTP_GUARD_MAX_ACTIVE_REQUESTS 4
#define HTTP_GUARD_MAX_CLIENTS 16
#define HTTP_GUARD_RATE_PER_SEC 12
#define HTTP_GUARD_BURST 24
#define HTTP_GUARD_MAX_INFLIGHT_PER_CLIENT_GET 3
#define HTTP_GUARD_MAX_INFLIGHT_PER_CLIENT_NON_GET 1

typedef struct {
    bool used;
    uint32_t key;
    int32_t tokens_milli;
    int64_t last_refill_ms;
    int64_t last_seen_ms;
} http_guard_bucket_t;

typedef struct {
    bool used;
    uint32_t key;
    uint8_t in_flight;
    int64_t last_seen_ms;
} http_guard_active_t;

static SemaphoreHandle_t s_active_sem = NULL;
static SemaphoreHandle_t s_guard_lock = NULL;
static bool s_inited = false;
static http_guard_bucket_t s_buckets[HTTP_GUARD_MAX_CLIENTS];
static http_guard_active_t s_active_clients[HTTP_GUARD_MAX_CLIENTS];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t req_client_key(httpd_req_t *req)
{
    if (req == NULL) {
        return 0;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return 0;
    }

    struct sockaddr_storage addr = {0};
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) != 0) {
        return 0;
    }

    if (addr.ss_family == AF_INET && addr_len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)&addr;
        return sa->sin_addr.s_addr;
    }

#if LWIP_IPV6
    if (addr.ss_family == AF_INET6 && addr_len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)&addr;
        const uint8_t *a = (const uint8_t *)&sa6->sin6_addr;
        uint32_t h = 2166136261u;
        for (size_t i = 0; i < 16; i++) {
            h ^= a[i];
            h *= 16777619u;
        }
        return h;
    }
#endif

    return 0;
}

static bool is_api_request(const httpd_req_t *req)
{
    if (req == NULL || req->uri[0] == '\0') {
        return false;
    }
    return strncmp(req->uri, "/api/", 5) == 0;
}

/* ---- request tracing ----------------------------------------------------
 * Every request that reaches a guarded handler is traced.  Plain successful
 * GETs (the WebUI/HA polling traffic) go to ESP_LOGD so they only appear at
 * verbosity >= 4, while anything notable - a write, a rejection, a failing
 * handler or a slow request - is recorded in the persistent log immediately
 * and handed to the screen flash detector, so a flash seen seconds after a
 * request can be attributed to it. */
static const char *http_method_name(int method)
{
    switch (method) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_HEAD: return "HEAD";
    case HTTP_OPTIONS: return "OPTIONS";
    case HTTP_PATCH: return "PATCH";
    default: return "OTHER";
    }
}

static void req_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    out[0] = '\0';
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
        inet_ntoa_r(((const struct sockaddr_in *)&addr)->sin_addr, out, out_len);
    }
#if LWIP_IPV6
    else if (addr.ss_family == AF_INET6 && addr_len >= sizeof(struct sockaddr_in6)) {
        inet6_ntoa_r(((const struct sockaddr_in6 *)&addr)->sin6_addr, out, out_len);
    }
#endif
}

static void req_query_suffix(httpd_req_t *req, char *out, size_t out_len)
{
    out[0] = '\0';
    if (out_len < 3) {
        return;
    }
    size_t qlen = httpd_req_get_url_query_len(req);
    char query[160];
    if (qlen == 0 || qlen >= sizeof(query) || (qlen + 2) > out_len) {
        return;
    }
    if (httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK) {
        snprintf(out, out_len, "?%.*s", (int)(out_len - 2), query);
    }
}

static void log_request_result(httpd_req_t *req, esp_err_t err, int64_t elapsed_ms, const char *reject_reason)
{
    char ip[48];
    char query[128];
    req_client_ip(req, ip, sizeof(ip));
    req_query_suffix(req, query, sizeof(query));

    const char *method = http_method_name(req->method);
    bool notable = (reject_reason != NULL) || (err != ESP_OK) || (req->method != HTTP_GET) ||
                   (elapsed_ms >= APP_HTTP_SLOW_LOG_MS);

    if (!notable) {
        ESP_LOGD(TAG_HTTP, "%s %s%s from %s len=%u -> %s in %lld ms", method, req->uri, query,
                 ip[0] != '\0' ? ip : "?", (unsigned)req->content_len, esp_err_to_name(err),
                 (long long)elapsed_ms);
        return;
    }

    if (reject_reason != NULL) {
        system_log_event(TAG_HTTP, "%s %s%s from %s REJECTED (%s)", method, req->uri, query,
                         ip[0] != '\0' ? ip : "?", reject_reason);
        return;
    }

    system_log_event(TAG_HTTP, "%s %s%s from %s len=%u -> %s in %lld ms", method, req->uri, query,
                     ip[0] != '\0' ? ip : "?", (unsigned)req->content_len, esp_err_to_name(err),
                     (long long)elapsed_ms);
}

static bool rate_limit_allow(uint32_t client_key)
{
    const int32_t burst_milli = HTTP_GUARD_BURST * 1000;
    const int64_t t_now = now_ms();
    int idx = -1;
    int free_idx = -1;
    int oldest_idx = -1;
    int64_t oldest_seen = INT64_MAX;

    for (int i = 0; i < HTTP_GUARD_MAX_CLIENTS; i++) {
        if (s_buckets[i].used) {
            if (s_buckets[i].key == client_key) {
                idx = i;
                break;
            }
            if (s_buckets[i].last_seen_ms < oldest_seen) {
                oldest_seen = s_buckets[i].last_seen_ms;
                oldest_idx = i;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }

    if (idx < 0) {
        idx = (free_idx >= 0) ? free_idx : oldest_idx;
        if (idx < 0) {
            return false;
        }
        s_buckets[idx].used = true;
        s_buckets[idx].key = client_key;
        s_buckets[idx].tokens_milli = burst_milli;
        s_buckets[idx].last_refill_ms = t_now;
        s_buckets[idx].last_seen_ms = t_now;
    }

    http_guard_bucket_t *b = &s_buckets[idx];
    int64_t elapsed_ms = t_now - b->last_refill_ms;
    if (elapsed_ms > 0) {
        int64_t refill_milli = elapsed_ms * HTTP_GUARD_RATE_PER_SEC;
        int64_t tokens = (int64_t)b->tokens_milli + refill_milli;
        if (tokens > burst_milli) {
            tokens = burst_milli;
        }
        b->tokens_milli = (int32_t)tokens;
        b->last_refill_ms = t_now;
    }
    b->last_seen_ms = t_now;

    if (b->tokens_milli < 1000) {
        return false;
    }

    b->tokens_milli -= 1000;
    return true;
}

static bool active_try_acquire(uint32_t client_key, uint8_t in_flight_limit)
{
    const int64_t t_now = now_ms();
    int idx = -1;
    int free_idx = -1;
    int oldest_idx = -1;
    int64_t oldest_seen = INT64_MAX;

    for (int i = 0; i < HTTP_GUARD_MAX_CLIENTS; i++) {
        if (s_active_clients[i].used) {
            if (s_active_clients[i].key == client_key) {
                idx = i;
                break;
            }
            if (s_active_clients[i].in_flight == 0 && s_active_clients[i].last_seen_ms < oldest_seen) {
                oldest_seen = s_active_clients[i].last_seen_ms;
                oldest_idx = i;
            }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }

    if (idx < 0) {
        idx = (free_idx >= 0) ? free_idx : oldest_idx;
        if (idx < 0) {
            return false;
        }
        s_active_clients[idx].used = true;
        s_active_clients[idx].key = client_key;
        s_active_clients[idx].in_flight = 0;
    }

    if (s_active_clients[idx].in_flight >= in_flight_limit) {
        s_active_clients[idx].last_seen_ms = t_now;
        return false;
    }

    s_active_clients[idx].in_flight++;
    s_active_clients[idx].last_seen_ms = t_now;
    return true;
}

static void active_release(uint32_t client_key)
{
    const int64_t t_now = now_ms();
    for (int i = 0; i < HTTP_GUARD_MAX_CLIENTS; i++) {
        if (!s_active_clients[i].used || s_active_clients[i].key != client_key) {
            continue;
        }
        if (s_active_clients[i].in_flight > 0) {
            s_active_clients[i].in_flight--;
        }
        s_active_clients[i].last_seen_ms = t_now;
        return;
    }
}

static esp_err_t send_busy(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Retry-After", "1");
    return httpd_resp_sendstr(req, message);
}

esp_err_t http_guard_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    if (s_guard_lock == NULL) {
        s_guard_lock = xSemaphoreCreateMutex();
    }
    if (s_guard_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_guard_lock, portMAX_DELAY);
    if (!s_inited) {
        if (s_active_sem == NULL) {
            s_active_sem = xSemaphoreCreateCounting(HTTP_GUARD_MAX_ACTIVE_REQUESTS, HTTP_GUARD_MAX_ACTIVE_REQUESTS);
        }
        if (s_active_sem == NULL) {
            xSemaphoreGive(s_guard_lock);
            return ESP_ERR_NO_MEM;
        }
        for (int i = 0; i < HTTP_GUARD_MAX_CLIENTS; i++) {
            s_buckets[i].used = false;
            s_active_clients[i].used = false;
        }
        s_inited = true;
    }
    xSemaphoreGive(s_guard_lock);
    return ESP_OK;
}

esp_err_t http_guard_handle(httpd_req_t *req, http_guard_handler_t next_handler)
{
    if (req == NULL || next_handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t init_err = http_guard_init();
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG_HTTP, "HTTP guard init failed: %s", esp_err_to_name(init_err));
        log_request_result(req, ESP_ERR_NO_MEM, 0, "guard init failed");
        return send_busy(req, "503 Service Unavailable", "Service unavailable");
    }

    uint32_t key = req_client_key(req);
    bool allow_rate = true;
    bool allow_concurrency = true;
    bool enforce_api_limits = is_api_request(req);
    uint8_t in_flight_limit = HTTP_GUARD_MAX_INFLIGHT_PER_CLIENT_NON_GET;
    if (req->method == HTTP_GET) {
        in_flight_limit = HTTP_GUARD_MAX_INFLIGHT_PER_CLIENT_GET;
    }

    xSemaphoreTake(s_guard_lock, portMAX_DELAY);
    if (enforce_api_limits) {
        allow_rate = rate_limit_allow(key);
        if (allow_rate) {
            allow_concurrency = active_try_acquire(key, in_flight_limit);
        }
    }
    xSemaphoreGive(s_guard_lock);

    if (!allow_rate) {
        log_request_result(req, ESP_ERR_INVALID_STATE, 0, "rate limit");
        return send_busy(req, "429 Too Many Requests", "Too many requests");
    }
    if (!allow_concurrency) {
        log_request_result(req, ESP_ERR_INVALID_STATE, 0, "too many concurrent requests");
        return send_busy(req, "429 Too Many Requests", "Too many concurrent requests");
    }

    if (xSemaphoreTake(s_active_sem, 0) != pdTRUE) {
        if (enforce_api_limits) {
            xSemaphoreTake(s_guard_lock, portMAX_DELAY);
            active_release(key);
            xSemaphoreGive(s_guard_lock);
        }
        log_request_result(req, ESP_ERR_TIMEOUT, 0, "server busy");
        return send_busy(req, "503 Service Unavailable", "Server busy");
    }

    /* The httpd runs its handlers from a single task, so this marker names the
     * request that is blocking the whole API if the panel ever freezes.  It is
     * left set on purpose when a handler never returns. */
    int64_t started_ms = esp_timer_get_time() / 1000;
    system_log_note_http(req->uri);
    esp_err_t err = next_handler(req);
    system_log_note_http(NULL);

    int64_t elapsed_ms = (esp_timer_get_time() / 1000) - started_ms;
    if (elapsed_ms >= APP_HTTP_SLOW_LOG_MS) {
        ESP_LOGW(TAG_HTTP, "slow request %s %s took %lld ms",
                 (req->method == HTTP_GET) ? "GET" : ((req->method == HTTP_POST) ? "POST" : "OTHER"),
                 req->uri, (long long)elapsed_ms);
    }
    log_request_result(req, err, elapsed_ms, NULL);

    xSemaphoreGive(s_active_sem);
    if (enforce_api_limits) {
        xSemaphoreTake(s_guard_lock, portMAX_DELAY);
        active_release(key);
        xSemaphoreGive(s_guard_lock);
    }
    return err;
}
