/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"

#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "app_events.h"
#include "drivers/display_init.h"
#include "ui/ui_page_transition.h"
#include "ui/ui_pages.h"

#define PAGES_LOCK_TIMEOUT_MS 1500U
#define PAGES_BODY_MAX_LEN 256U
#define PAGES_EVENT_TIMEOUT_TICKS pdMS_TO_TICKS(100)

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", (message != NULL) ? message : "Invalid request");
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static esp_err_t send_json_document(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t api_pages_get_handler(httpd_req_t *req)
{
    if (!display_lock(PAGES_LOCK_TIMEOUT_MS)) {
        return send_json_error(req, "503 Service Unavailable", "Display is busy");
    }

    const uint16_t count = ui_pages_count();
    char current[APP_MAX_PAGE_ID_LEN] = {0};
    strlcpy(current, ui_pages_current_id(), sizeof(current));
    char transition_mode[APP_DISPLAY_PAGE_TRANSITION_MAX_LEN] = {0};
    strlcpy(transition_mode, ui_page_transition_mode(), sizeof(transition_mode));
    const uint16_t transition_ms = ui_page_transition_duration_ms();

    cJSON *pages = cJSON_CreateArray();
    if (pages != NULL) {
        for (uint16_t i = 0; i < count; i++) {
            const char *id = ui_pages_id_at(i);
            cJSON *page = cJSON_CreateObject();
            if (page == NULL) {
                cJSON_Delete(pages);
                pages = NULL;
                break;
            }
            cJSON_AddNumberToObject(page, "index", i);
            cJSON_AddStringToObject(page, "id", id);
            cJSON_AddStringToObject(page, "title", ui_pages_title_at(i));
            cJSON_AddBoolToObject(page, "active", strcmp(id, current) == 0);
            cJSON_AddItemToArray(pages, page);
        }
    }
    display_unlock();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL || pages == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(pages);
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "current", current);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddItemToObject(root, "pages", pages);
    cJSON *transition = cJSON_CreateObject();
    if (transition != NULL) {
        cJSON_AddStringToObject(transition, "mode", transition_mode);
        cJSON_AddNumberToObject(transition, "duration_ms", transition_ms);
        cJSON_AddItemToObject(root, "transition", transition);
    }
    return send_json_document(req, root);
}

/* Resolves the requested target page and returns its index, or -1. */
static int resolve_target_index(cJSON *body, int *out_index)
{
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(body, "id");
    cJSON *index_item = cJSON_GetObjectItemCaseSensitive(body, "index");
    cJSON *next_item = cJSON_GetObjectItemCaseSensitive(body, "next");

    if (cJSON_IsNumber(index_item) && !cJSON_IsString(id_item)) {
        int index = (int)index_item->valuedouble;
        if (index < 0 || index >= (int)ui_pages_count()) {
            return -1;
        }
        *out_index = index;
        return 0;
    }
    if (cJSON_IsString(id_item) && id_item->valuestring != NULL) {
        for (uint16_t i = 0; i < ui_pages_count(); i++) {
            if (strcmp(ui_pages_id_at(i), id_item->valuestring) == 0) {
                *out_index = (int)i;
                return 0;
            }
        }
        return -1;
    }
    if (cJSON_IsTrue(next_item)) {
        const uint16_t count = ui_pages_count();
        if (count == 0) {
            return -1;
        }
        const char *current = ui_pages_current_id();
        int current_index = -1;
        for (uint16_t i = 0; i < count; i++) {
            if (strcmp(ui_pages_id_at(i), current) == 0) {
                current_index = (int)i;
                break;
            }
        }
        if (current_index < 0) {
            return -1;
        }
        *out_index = (current_index + 1) % (int)count;
        return 0;
    }
    return -1;
}

esp_err_t api_pages_activate_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > (int)PAGES_BODY_MAX_LEN) {
        return send_json_error(req, "400 Bad Request", "Expected a JSON body with id, index or next");
    }

    char body[PAGES_BODY_MAX_LEN + 1] = {0};
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return send_json_error(req, "400 Bad Request", "Failed to read request body");
        }
        received += r;
    }

    cJSON *parsed = cJSON_Parse(body);
    if (!cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        return send_json_error(req, "400 Bad Request", "Body must be a JSON object");
    }

    int target_index = -1;
    if (!display_lock(PAGES_LOCK_TIMEOUT_MS)) {
        cJSON_Delete(parsed);
        return send_json_error(req, "503 Service Unavailable", "Display is busy");
    }
    const int resolve_err = resolve_target_index(parsed, &target_index);
    char target_id[APP_MAX_PAGE_ID_LEN] = {0};
    if (resolve_err == 0) {
        strlcpy(target_id, ui_pages_id_at((uint16_t)target_index), sizeof(target_id));
    }
    display_unlock();
    cJSON_Delete(parsed);

    if (resolve_err != 0 || target_id[0] == '\0') {
        return send_json_error(req, "404 Not Found", "Unknown page; use an id or index from GET /api/pages");
    }

    app_event_t event = {0};
    event.type = EV_UI_NAVIGATE;
    strlcpy(event.data.navigate.page_id, target_id, sizeof(event.data.navigate.page_id));
    if (!app_events_publish(&event, PAGES_EVENT_TIMEOUT_TICKS)) {
        return send_json_error(req, "503 Service Unavailable", "UI event queue is busy");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "page", target_id);
    cJSON_AddNumberToObject(root, "index", target_index);
    return send_json_document(req, root);
}
