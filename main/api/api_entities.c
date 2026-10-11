/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "radar/ld2410.h"
#include "drivers/display_init.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "ha/ha_client.h"
#include "ha/ha_model.h"
#include "ui/ui_mdi_icon.h"

#define API_ENTITIES_MAX_ITEMS_DEFAULT 128U
#define API_ENTITIES_MAX_ITEMS_MIN 8U
#define API_ENTITIES_MAX_ITEMS_MAX 128U

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static size_t parse_max_items(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return API_ENTITIES_MAX_ITEMS_DEFAULT;
    }

    char *end_ptr = NULL;
    long parsed = strtol(value, &end_ptr, 10);
    if (end_ptr == value || *end_ptr != '\0' || parsed <= 0) {
        return API_ENTITIES_MAX_ITEMS_DEFAULT;
    }
    if ((size_t)parsed < API_ENTITIES_MAX_ITEMS_MIN) {
        return API_ENTITIES_MAX_ITEMS_MIN;
    }
    if ((size_t)parsed > API_ENTITIES_MAX_ITEMS_MAX) {
        return API_ENTITIES_MAX_ITEMS_MAX;
    }
    return (size_t)parsed;
}

esp_err_t api_entities_get_handler(httpd_req_t *req)
{
    char domain[APP_MAX_NAME_LEN] = {0};
    char search[APP_MAX_NAME_LEN] = {0};
    char limit_raw[12] = {0};

    int query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char *query = calloc((size_t)query_len + 1U, sizeof(char));
        if (query == NULL) {
            return httpd_resp_send_500(req);
        }
        if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            httpd_query_key_value(query, "domain", domain, sizeof(domain));
            httpd_query_key_value(query, "search", search, sizeof(search));
            httpd_query_key_value(query, "limit", limit_raw, sizeof(limit_raw));
        }
        free(query);
    }

    const size_t max_items = parse_max_items(limit_raw);
    ha_entity_info_t *items = calloc(max_items, sizeof(ha_entity_info_t));
    if (items == NULL) {
        return httpd_resp_send_500(req);
    }
    size_t count = ha_model_list_entities(
        domain[0] ? domain : NULL, search[0] ? search : NULL, items, max_items);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (root == NULL || arr == NULL) {
        free(items);
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return httpd_resp_send_500(req);
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "id", items[i].id);
        cJSON_AddStringToObject(it, "name", items[i].name);
        cJSON_AddStringToObject(it, "domain", items[i].domain);
        cJSON_AddStringToObject(it, "unit", items[i].unit);
        cJSON_AddStringToObject(it, "device_class", items[i].device_class);
        cJSON_AddNumberToObject(it, "supported_features", (double)items[i].supported_features);
        cJSON *capabilities = cJSON_CreateObject();
        if (capabilities != NULL) {
            cJSON_AddBoolToObject(capabilities, "dimming", items[i].supports_dimming);
            cJSON_AddBoolToObject(capabilities, "color", items[i].supports_color);
            cJSON_AddBoolToObject(capabilities, "color_temp", items[i].supports_color_temp);
            cJSON_AddItemToObject(it, "capabilities", capabilities);
        }
        cJSON_AddStringToObject(it, "icon", items[i].icon);
        cJSON_AddItemToArray(arr, it);
    }
    free(items);

    cJSON_AddItemToObject(root, "items", arr);
    cJSON_AddNumberToObject(root, "count", (double)count);
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

esp_err_t api_light_entities_get_handler(httpd_req_t *req)
{
    char refresh_raw[8] = {0};
    char domain[APP_HA_DISCOVERY_DOMAIN_MAX_LEN] = "light";
    char search[APP_HA_DISCOVERY_SEARCH_MAX_LEN] = {0};

    int query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char *query = calloc((size_t)query_len + 1U, sizeof(char));
        if (query == NULL) {
            return httpd_resp_send_500(req);
        }
        if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            httpd_query_key_value(query, "refresh", refresh_raw, sizeof(refresh_raw));
            httpd_query_key_value(query, "domain", domain, sizeof(domain));
            httpd_query_key_value(query, "search", search, sizeof(search));
        }
        free(query);
    }

    bool refresh = strcmp(refresh_raw, "1") == 0 || strcmp(refresh_raw, "true") == 0;
    char *payload = NULL;
    esp_err_t err = ha_client_get_domain_entities_json(domain, search, refresh, &payload);
    if (err != ESP_OK || payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t api_light_entities_delete_handler(httpd_req_t *req)
{
    char domain[APP_HA_DISCOVERY_DOMAIN_MAX_LEN] = "light";
    char search[APP_HA_DISCOVERY_SEARCH_MAX_LEN] = {0};

    int query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char *query = calloc((size_t)query_len + 1U, sizeof(char));
        if (query == NULL) {
            return httpd_resp_send_500(req);
        }
        if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            httpd_query_key_value(query, "domain", domain, sizeof(domain));
            httpd_query_key_value(query, "search", search, sizeof(search));
        }
        free(query);
    }

    esp_err_t err = ha_client_cancel_domain_entities(domain, search);
    if (err != ESP_OK) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* GET /api/mdi/icons?q=desk&limit=40 -> {"items":["desk","desk-lamp",...]}
 * Name search over the embedded Material Design Icons for the editor's icon
 * picker (prefix matches first). */
esp_err_t api_mdi_icons_get_handler(httpd_req_t *req)
{
    char query[48] = {0};
    char limit_raw[8] = {0};
    int query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char *qs = (char *)malloc((size_t)query_len + 1U);
        if (qs != NULL) {
            if (httpd_req_get_url_query_str(req, qs, (size_t)query_len + 1U) == ESP_OK) {
                httpd_query_key_value(qs, "q", query, sizeof(query));
                httpd_query_key_value(qs, "limit", limit_raw, sizeof(limit_raw));
            }
            free(qs);
        }
    }
    size_t limit = (size_t)strtoul(limit_raw, NULL, 10);
    if (limit == 0 || limit > 100) {
        limit = 40;
    }

    const char *names[100];
    const size_t count = ui_mdi_icon_search(query, names, limit);

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(items, cJSON_CreateString(names[i]));
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

/* GET /api/assist/pipelines -> {"pipelines":[{"id","name","language"}],"preferred":"..."}
 * Asks Home Assistant over the websocket and waits for the answer. */
static SemaphoreHandle_t s_pipelines_done;
static uint32_t s_pipelines_gen;
static char *s_pipelines_json;

static void api_assist_pipelines_cb(bool success, cJSON *result, void *user)
{
    if ((uint32_t)(uintptr_t)user != s_pipelines_gen) {
        return; /* answer of a request that already timed out */
    }
    cJSON *out = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(out, "pipelines");
    cJSON *pipelines = success ? cJSON_GetObjectItemCaseSensitive(result, "pipelines") : NULL;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, pipelines)
    {
        cJSON *entry = cJSON_CreateObject();
        const char *keys[] = {"id", "name", "language", "stt_engine", "tts_engine"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(item, keys[i]);
            cJSON_AddStringToObject(entry, keys[i], cJSON_IsString(v) ? v->valuestring : "");
        }
        cJSON_AddItemToArray(list, entry);
    }
    cJSON *preferred = success ? cJSON_GetObjectItemCaseSensitive(result, "preferred_pipeline") : NULL;
    cJSON_AddStringToObject(out, "preferred", cJSON_IsString(preferred) ? preferred->valuestring : "");
    cJSON_AddBoolToObject(out, "ok", success);
    s_pipelines_json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    xSemaphoreGive(s_pipelines_done);
}

esp_err_t api_assist_pipelines_get_handler(httpd_req_t *req)
{
    if (s_pipelines_done == NULL) {
        s_pipelines_done = xSemaphoreCreateBinary();
        if (s_pipelines_done == NULL) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        }
    }
    (void)xSemaphoreTake(s_pipelines_done, 0);
    if (s_pipelines_json != NULL) {
        cJSON_free(s_pipelines_json);
        s_pipelines_json = NULL;
    }
    const uint32_t gen = ++s_pipelines_gen;
    esp_err_t err = ha_client_list_assist_pipelines(api_assist_pipelines_cb, (void *)(uintptr_t)gen);
    const char *fallback = "{\"pipelines\":[],\"preferred\":\"\",\"ok\":false}";
    set_json_headers(req);
    if (err != ESP_OK || xSemaphoreTake(s_pipelines_done, pdMS_TO_TICKS(5000)) != pdTRUE || s_pipelines_json == NULL) {
        s_pipelines_gen++;
        return httpd_resp_sendstr(req, fallback);
    }
    err = httpd_resp_sendstr(req, s_pipelines_json);
    cJSON_free(s_pipelines_json);
    s_pipelines_json = NULL;
    return err;
}

/* GET /api/debug/activity -> what keeps resetting the screensaver's idle timer. */
esp_err_t api_debug_activity_get_handler(httpd_req_t *req)
{
    display_activity_source_t sources[12];
    const size_t n = display_get_activity_sources(sources, sizeof(sources) / sizeof(sources[0]));
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    cJSON_AddNumberToObject(root, "idle_ms", (double)display_ms_since_activity());
    /* Render cost since boot: compare total_ms / passes before and after a change. */
    display_render_stats_t rs;
    display_render_stats_get(&rs);
    cJSON *render = cJSON_AddObjectToObject(root, "render");
    if (render != NULL) {
        cJSON_AddNumberToObject(render, "passes", rs.passes);
        cJSON_AddNumberToObject(render, "full_passes", rs.full_passes);
        cJSON_AddNumberToObject(render, "total_ms", (double)(rs.total_pass_us / 1000));
        cJSON_AddNumberToObject(render, "max_ms", rs.max_pass_ms);
    }
    cJSON *list = cJSON_AddArrayToObject(root, "sources");
    for (size_t i = 0; i < n && list != NULL; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            break;
        }
        cJSON_AddStringToObject(item, "source", sources[i].source);
        cJSON_AddNumberToObject(item, "count", sources[i].count);
        cJSON_AddNumberToObject(item, "ago_ms", (double)sources[i].ago_ms);
        cJSON_AddItemToArray(list, item);
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

/* GET /api/radar -> live state of the LD2410 presence radar. */
esp_err_t api_radar_get_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char selftest_msg[96] = {0};
    bool selftest = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "selftest=1") != NULL) {
        selftest = true;
        (void)ld2410_self_test(selftest_msg, sizeof(selftest_msg));
    }
    ld2410_status_t st;
    ld2410_get_status(&st);
    static const char *const targets[] = {"none", "moving", "static", "both"};
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", st.enabled);
    cJSON_AddBoolToObject(root, "running", st.running);
    cJSON_AddBoolToObject(root, "detected", st.detected);
    cJSON_AddStringToObject(root, "error", st.error);
    cJSON_AddNumberToObject(root, "frames", st.frames);
    cJSON_AddNumberToObject(root, "bad_frames", st.bad_frames);
    cJSON_AddNumberToObject(root, "rx_bytes", st.rx_bytes);
    char hex[3 * 16 + 1] = {0};
    for (int i = 0; i < st.first_count; i++) {
        snprintf(hex + i * 3, sizeof(hex) - (size_t)i * 3, "%02X ", st.first_bytes[i]);
    }
    cJSON_AddStringToObject(root, "first_bytes", hex);
    if (st.last_frame_age_ms != UINT32_MAX) {
        cJSON_AddNumberToObject(root, "last_frame_age_ms", st.last_frame_age_ms);
    }
    cJSON_AddStringToObject(root, "target", targets[st.target & 3]);
    cJSON_AddNumberToObject(root, "moving_cm", st.moving_cm);
    cJSON_AddNumberToObject(root, "moving_energy", st.moving_energy);
    cJSON_AddNumberToObject(root, "static_cm", st.static_cm);
    cJSON_AddNumberToObject(root, "static_energy", st.static_energy);
    cJSON_AddNumberToObject(root, "detect_cm", st.detect_cm);
    cJSON_AddBoolToObject(root, "near", st.near);
    if (selftest) {
        cJSON_AddStringToObject(root, "selftest", selftest_msg);
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}
