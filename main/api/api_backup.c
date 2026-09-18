/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "diag/system_log.h"
#include "ha/ha_client.h"
#include "layout/layout_store.h"
#include "layout/layout_validate.h"
#include "mqtt/panel_mqtt.h"
#include "settings/runtime_settings.h"
#include "ui/theme/theme_palette.h"
#include "ui/theme/theme_store.h"
#include "ui/ui_runtime.h"
#include "ui/ui_screen_saver.h"
#include "ui/ui_page_transition.h"
#include "ui/ui_press_feedback.h"

#define BACKUP_VERSION 1

static const char *TAG = "api_backup";

static void set_headers(httpd_req_t *req, bool attachment)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (attachment) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"betta-ha-panel-backup.json\"");
    }
}

static esp_err_t send_payload(httpd_req_t *req, const char *payload, bool attachment)
{
    set_headers(req, attachment);
    return httpd_resp_sendstr(req, payload);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", (message != NULL) ? message : "error");
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }
    set_headers(req, false);
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static char *read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > APP_BACKUP_MAX_JSON_LEN) {
        return NULL;
    }
    char *buf = calloc((size_t)req->content_len + 1U, sizeof(char));
    if (buf == NULL) {
        return NULL;
    }
    size_t total = 0;
    while (total < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + total, (int)((size_t)req->content_len - total));
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        total += (size_t)r;
    }
    buf[total] = '\0';
    return buf;
}

/* GET /api/backup
 * -> { backup_version, created, app{...}, layout{...}, settings{...},
 *      themes{active_id, custom:[...]} }
 * Secrets (HA token, MQTT password, Wi-Fi credentials) are never exported. */
esp_err_t api_backup_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddNumberToObject(root, "backup_version", BACKUP_VERSION);
    cJSON_AddNumberToObject(root, "created", (double)(esp_timer_get_time() / 1000000));

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *app = cJSON_AddObjectToObject(root, "app");
    if (app != NULL) {
        cJSON_AddStringToObject(app, "name", APP_NAME);
        cJSON_AddStringToObject(
            app,
            "project",
            (desc != NULL && desc->project_name[0] != '\0') ? desc->project_name : APP_NAME);
        cJSON_AddStringToObject(
            app, "version", (desc != NULL && desc->version[0] != '\0') ? desc->version : "unknown");
    }

    char *layout_json = NULL;
    if (layout_store_load(&layout_json) != ESP_OK || layout_json == NULL) {
        layout_json = strdup(layout_store_default_json());
    }
    cJSON *layout = (layout_json != NULL) ? cJSON_Parse(layout_json) : NULL;
    free(layout_json);
    if (!cJSON_IsObject(layout)) {
        cJSON_Delete(layout);
        cJSON_Delete(root);
        return send_error(req, "500 Internal Server Error", "Failed to read layout");
    }
    cJSON_AddItemToObject(root, "layout", layout);

    runtime_settings_t *settings = calloc(1, sizeof(runtime_settings_t));
    if (settings == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    esp_err_t load_err = runtime_settings_load(settings);
    cJSON *settings_json = (load_err == ESP_OK) ? runtime_settings_public_json(settings) : NULL;
    free(settings);
    if (settings_json == NULL) {
        cJSON_Delete(root);
        return send_error(req, "500 Internal Server Error", "Failed to read settings");
    }
    cJSON_AddItemToObject(root, "settings", settings_json);

    cJSON *themes = cJSON_AddObjectToObject(root, "themes");
    cJSON *custom = (themes != NULL) ? cJSON_AddArrayToObject(themes, "custom") : NULL;
    if (themes == NULL || custom == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(themes, "active_id", theme_palette_active_id());

    char custom_ids[APP_MAX_CUSTOM_THEMES][APP_MAX_THEME_ID_LEN];
    size_t custom_count = 0;
    (void)theme_store_list_custom(custom_ids, APP_MAX_CUSTOM_THEMES, &custom_count);
    for (size_t i = 0; i < custom_count; i++) {
        theme_entry_t entry = {0};
        if (theme_store_load_custom(custom_ids[i], &entry) != ESP_OK) {
            continue;
        }
        char *theme_json = theme_palette_to_json(&entry);
        if (theme_json == NULL) {
            continue;
        }
        cJSON *theme_item = cJSON_Parse(theme_json);
        cJSON_free(theme_json);
        if (cJSON_IsObject(theme_item)) {
            cJSON_AddItemToArray(custom, theme_item);
        } else {
            cJSON_Delete(theme_item);
        }
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    ESP_LOGI(TAG, "Backup exported (%u bytes)", (unsigned)strlen(payload));
    system_log_write_info(TAG, "Backup exported (%u bytes)", (unsigned)strlen(payload));

    esp_err_t err = send_payload(req, payload, true);
    cJSON_free(payload);
    return err;
}

static const char *backup_sections_error(const cJSON *root)
{
    const cJSON *layout = cJSON_GetObjectItemCaseSensitive(root, "layout");
    if (cJSON_IsObject(layout)) {
        char *layout_json = cJSON_PrintUnformatted(layout);
        if (layout_json == NULL) {
            return "Backup layout is too large";
        }
        layout_validation_result_t validation;
        bool valid = layout_validate_json(layout_json, &validation);
        cJSON_free(layout_json);
        if (!valid) {
            return "Backup contains an invalid layout";
        }
    }

    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (cJSON_IsObject(settings)) {
        char *settings_json = cJSON_PrintUnformatted(settings);
        if (settings_json == NULL) {
            return "Backup settings are too large";
        }
        esp_err_t err = runtime_settings_validate_public_json(settings_json);
        cJSON_free(settings_json);
        if (err != ESP_OK) {
            return "Backup contains invalid settings";
        }
    }

    return NULL;
}

static esp_err_t restore_layout(httpd_req_t *req, const cJSON *layout, bool *out_ok)
{
    *out_ok = false;
    if (!cJSON_IsObject(layout)) {
        return ESP_OK;
    }

    char *layout_json = cJSON_PrintUnformatted(layout);
    if (layout_json == NULL) {
        return httpd_resp_send_500(req);
    }

    layout_validation_result_t validation;
    if (!layout_validate_json(layout_json, &validation)) {
        cJSON_free(layout_json);
        return send_error(req, "400 Bad Request", "Backup contains an invalid layout");
    }

    esp_err_t err = layout_store_save(layout_json);
    cJSON_free(layout_json);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Failed to store the restored layout");
    }

    esp_err_t apply_err = ui_runtime_request_layout_reload();
    if (apply_err != ESP_OK) {
        /* The layout is stored in NVS and will be used on the next boot, but the
         * panel did not rebuild now — report it instead of pretending it did. */
        ESP_LOGW(TAG, "Restored layout stored but UI rebuild failed: %s", esp_err_to_name(apply_err));
    }
    if (ha_client_notify_layout_updated() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to notify HA client about restored layout");
    }
    *out_ok = (apply_err == ESP_OK);
    return ESP_OK;
}

static size_t restore_themes(const cJSON *themes)
{
    if (!cJSON_IsObject(themes)) {
        return 0;
    }

    const cJSON *custom = cJSON_GetObjectItemCaseSensitive(themes, "custom");
    size_t restored = 0;
    if (cJSON_IsArray(custom)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, custom) {
            if (!cJSON_IsObject(item)) {
                continue;
            }
            char *theme_json = cJSON_PrintUnformatted(item);
            if (theme_json == NULL) {
                continue;
            }
            theme_entry_t entry = {0};
            bool parsed = theme_palette_from_json(theme_json, &entry);
            cJSON_free(theme_json);
            if (!parsed || entry.id[0] == '\0') {
                continue;
            }
            entry.builtin = false;
            if (theme_store_save_custom(&entry) == ESP_OK) {
                restored++;
            }
        }
    }

    const cJSON *active = cJSON_GetObjectItemCaseSensitive(themes, "active_id");
    if (cJSON_IsString(active) && active->valuestring != NULL && active->valuestring[0] != '\0') {
        if (theme_palette_activate_by_id(active->valuestring) == ESP_OK) {
            (void)theme_store_set_active_id(active->valuestring);
            if (ui_runtime_request_layout_reload() != ESP_OK) {
                ESP_LOGW(TAG, "Restored active theme stored but UI rebuild failed");
            }
        } else {
            ESP_LOGW(TAG, "Backup active theme '%s' is unavailable, keeping current theme", active->valuestring);
        }
    }

    return restored;
}

/* POST /api/backup/restore
 * Accepts a document produced by GET /api/backup. Wi-Fi credentials and all
 * secrets stay untouched so a broken backup can never lock the panel out. */
esp_err_t api_backup_restore_post_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (body == NULL) {
        return send_error(req, "400 Bad Request", "Invalid or too large backup payload");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", "Backup must be a JSON object");
    }

    /* Everything is checked before the first write so a broken backup file
     * cannot leave the panel half restored. */
    const char *reject = backup_sections_error(root);
    if (reject != NULL) {
        cJSON_Delete(root);
        return send_error(req, "400 Bad Request", reject);
    }

    cJSON *settings_section = cJSON_GetObjectItemCaseSensitive(root, "settings");
    bool settings_restored = false;
    bool restart_required = false;
    if (cJSON_IsObject(settings_section)) {
        char *settings_json = cJSON_PrintUnformatted(settings_section);
        if (settings_json == NULL) {
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }
        esp_err_t settings_err = runtime_settings_apply_public_json(settings_json, &restart_required);
        cJSON_free(settings_json);
        if (settings_err != ESP_OK) {
            cJSON_Delete(root);
            return send_error(req, "400 Bad Request", "Backup contains invalid settings");
        }
        settings_restored = true;

        runtime_settings_t *applied = calloc(1, sizeof(runtime_settings_t));
        if (applied != NULL) {
            if (runtime_settings_load(applied) == ESP_OK) {
                ui_screen_saver_apply_settings(applied);
                ui_page_transition_apply_settings(applied);
                ui_press_feedback_apply_settings(applied);
                system_log_set_verbosity(applied->log_verbosity);
                system_log_event("settings", "log_verbosity=%d (restore)", applied->log_verbosity);
                panel_mqtt_notify_settings_changed();
            }
            free(applied);
        }
    }

    bool layout_restored = false;
    esp_err_t layout_err = restore_layout(req, cJSON_GetObjectItemCaseSensitive(root, "layout"), &layout_restored);
    if (layout_err != ESP_OK) {
        cJSON_Delete(root);
        return layout_err;
    }

    size_t themes_restored = restore_themes(cJSON_GetObjectItemCaseSensitive(root, "themes"));
    cJSON_Delete(root);

    system_log_write_info(
        TAG,
        "Backup restored (layout=%d settings=%d themes=%u%s)",
        layout_restored ? 1 : 0,
        settings_restored ? 1 : 0,
        (unsigned)themes_restored,
        restart_required ? ", restart recommended" : "");
    ESP_LOGI(
        TAG,
        "Backup restored (layout=%d settings=%d themes=%u restart=%d)",
        layout_restored ? 1 : 0,
        settings_restored ? 1 : 0,
        (unsigned)themes_restored,
        restart_required ? 1 : 0);

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "layout_restored", layout_restored);
    cJSON_AddBoolToObject(resp, "settings_restored", settings_restored);
    cJSON_AddNumberToObject(resp, "themes_restored", (double)themes_restored);
    cJSON_AddBoolToObject(resp, "restart_required", restart_required);
    char *payload = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = send_payload(req, payload, false);
    cJSON_free(payload);
    return err;
}
