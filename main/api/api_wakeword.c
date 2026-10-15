/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
/* Wake word: status, installed models, install from a manifest URL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"

#include "api_routes.h"
#include "assist/panel_assist.h"
#include "assist/panel_wake.h"

static esp_err_t wake_send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return httpd_resp_send_500(req);
    }
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t wake_send_result(httpd_req_t *req, esp_err_t res, const char *id, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", res == ESP_OK);
    if (id != NULL && id[0] != '\0') {
        cJSON_AddStringToObject(root, "id", id);
    }
    if (res != ESP_OK) {
        cJSON_AddStringToObject(root, "error", error != NULL && error[0] != '\0' ? error : esp_err_to_name(res));
    }
    return wake_send_json(req, res == ESP_OK ? NULL : "400 Bad Request", root);
}

/* GET /api/wakeword */
esp_err_t api_wakeword_get_handler(httpd_req_t *req)
{
    panel_wake_status_t st;
    panel_wake_get_status(&st);
    panel_wake_model_t *models = calloc(PANEL_WAKE_MODELS_MAX, sizeof(*models));
    cJSON *root = cJSON_CreateObject();
    if (root == NULL || models == NULL) {
        free(models);
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "enabled", st.enabled);
    cJSON_AddBoolToObject(root, "loaded", st.loaded);
    cJSON_AddBoolToObject(root, "listening", st.listening);
    cJSON_AddStringToObject(root, "model", st.model);
    cJSON_AddStringToObject(root, "wake_word", st.wake_word);
    cJSON_AddStringToObject(root, "error", st.error);
    cJSON_AddNumberToObject(root, "detections", st.detections);
    const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "last_detection_ago_s", st.last_detection_s != 0 ? (double)(now_s - st.last_detection_s) : -1);
    cJSON_AddNumberToObject(root, "invoke_us", st.invoke_us);
    cJSON_AddNumberToObject(root, "peak", st.peak / 255.0);
    cJSON_AddNumberToObject(root, "level_db", st.level_db);
    cJSON *list = cJSON_AddArrayToObject(root, "models");
    const int n = panel_wake_list_models(models, PANEL_WAKE_MODELS_MAX);
    for (int i = 0; i < n && list != NULL; i++) {
        cJSON *m = cJSON_CreateObject();
        if (m == NULL) {
            break;
        }
        cJSON_AddStringToObject(m, "id", models[i].id);
        cJSON_AddStringToObject(m, "wake_word", models[i].wake_word);
        cJSON_AddBoolToObject(m, "builtin", models[i].builtin);
        cJSON_AddNumberToObject(m, "cutoff", models[i].cutoff);
        cJSON_AddNumberToObject(m, "window", models[i].window);
        cJSON_AddItemToArray(list, m);
    }
    free(models);
    return wake_send_json(req, NULL, root);
}

/* POST /api/wakeword/install {"url": "https://.../hey_jarvis.json"} */
esp_err_t api_wakeword_install_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) {
        return wake_send_result(req, ESP_ERR_INVALID_SIZE, NULL, "invalid payload size");
    }
    char body[1025];
    int got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return wake_send_result(req, ESP_FAIL, NULL, "failed to read request body");
        }
        got += r;
    }
    body[got] = '\0';
    cJSON *root = cJSON_Parse(body);
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(url) || url->valuestring == NULL) {
        cJSON_Delete(root);
        return wake_send_result(req, ESP_ERR_INVALID_ARG, NULL, "\"url\" missing");
    }
    char id[RUNTIME_WAKE_MODEL_MAX_LEN] = {0};
    char err[96] = {0};
    const esp_err_t res = panel_wake_install(url->valuestring, id, sizeof(id), err, sizeof(err));
    cJSON_Delete(root);
    return wake_send_result(req, res, id, err);
}

/* DELETE /api/wakeword/model?id=hey_jarvis */
esp_err_t api_wakeword_model_delete_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char id[RUNTIME_WAKE_MODEL_MAX_LEN] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK) {
        return wake_send_result(req, ESP_ERR_INVALID_ARG, NULL, "id missing");
    }
    const esp_err_t res = panel_wake_delete(id);
    return wake_send_result(req, res, id, res == ESP_ERR_INVALID_ARG ? "the built-in model cannot be removed" : NULL);
}

/* POST /api/wakeword/test  body: raw 16 kHz mono s16le PCM (max 10 s) */
esp_err_t api_wakeword_test_post_handler(httpd_req_t *req)
{
    const int max = 16000 * 2 * 10;
    if (req->content_len < 3200 || req->content_len > max) {
        return wake_send_result(req, ESP_ERR_INVALID_SIZE, NULL, "0.1 to 10 s of 16 kHz mono PCM expected");
    }
    int16_t *pcm = heap_caps_malloc((size_t)req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        return httpd_resp_send_500(req);
    }
    int got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, (char *)pcm + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            heap_caps_free(pcm);
            return wake_send_result(req, ESP_FAIL, NULL, "failed to read request body");
        }
        got += r;
    }
    bool detected = false;
    uint8_t peak = 0;
    const esp_err_t res = panel_wake_test(pcm, (size_t)got / 2, &detected, &peak);
    heap_caps_free(pcm);
    if (res != ESP_OK) {
        return wake_send_result(req, res, NULL, res == ESP_ERR_INVALID_STATE ? "wake word off or model not loaded" : NULL);
    }
    /* ?start=1: a detection starts a real Assist session, as the microphone would */
    char query[32] = {0};
    char start[4] = {0};
    bool started = false;
    if (detected && httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "start", start, sizeof(start)) == ESP_OK && strcmp(start, "1") == 0) {
        started = panel_assist_start() == ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "detected", detected);
    cJSON_AddNumberToObject(root, "peak", peak / 255.0);
    cJSON_AddBoolToObject(root, "started", started);
    return wake_send_json(req, NULL, root);
}
