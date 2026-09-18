/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"

#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "diag/system_log.h"

#define LOG_TAIL_BYTES (APP_LOG_MAX_FILE_BYTES)
#define LOG_SEGMENT_MARKER "\n--- older log segment ---\n"

static bool logs_query_flag(httpd_req_t *req, const char *name)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }

    char value[8];
    if (httpd_query_key_value(query, name, value, sizeof(value)) != ESP_OK) {
        return false;
    }

    return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

static void logs_set_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

/* Stream every retained segment, oldest first.  The active file alone only holds
 * a few minutes at this verbosity, so a rare event (a screen flash, a freeze)
 * rolls out before anyone reads it; this keeps the whole history retrievable in
 * one request, with a marker where the older file ends and the newer begins. */
static esp_err_t logs_send_history(httpd_req_t *req)
{
    char *buf = malloc(LOG_TAIL_BYTES + 1U);
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }

    logs_set_headers(req);

    esp_err_t err = ESP_OK;
    bool sent_any = false;
    for (int rot = APP_LOG_MAX_ROTATED; rot >= 0 && err == ESP_OK; rot--) {
        int got = system_log_read_segment(rot, buf, LOG_TAIL_BYTES + 1U);
        if (got <= 0) {
            continue;
        }
        if (sent_any) {
            err = httpd_resp_send_chunk(req, LOG_SEGMENT_MARKER, HTTPD_RESP_USE_STRLEN);
            if (err != ESP_OK) {
                break;
            }
        }
        err = httpd_resp_send_chunk(req, buf, got);
        sent_any = true;
    }

    if (err == ESP_OK && !sent_any) {
        err = httpd_resp_send_chunk(req, "(no logs yet)\n", HTTPD_RESP_USE_STRLEN);
    }

    free(buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

esp_err_t api_logs_get_handler(httpd_req_t *req)
{
    if (logs_query_flag(req, "all")) {
        return logs_send_history(req);
    }

    char *buf = malloc(LOG_TAIL_BYTES + 1U);
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }

    int got = system_log_read_tail(buf, LOG_TAIL_BYTES + 1U);
    if (got <= 0) {
        strcpy(buf, "(no logs yet)\n");
    }

    logs_set_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, buf);
    free(buf);
    return err;
}

esp_err_t api_logs_delete_handler(httpd_req_t *req)
{
    esp_err_t err = system_log_clear();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_500(req);
    }

    logs_set_headers(req);
    return httpd_resp_sendstr(req, "ok\n");
}
