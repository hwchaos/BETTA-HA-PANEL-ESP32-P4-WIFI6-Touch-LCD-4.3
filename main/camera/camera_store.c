/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "camera/camera_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "util/json_util.h"
#include "util/log_tags.h"

#define TAG TAG_CAMERA

static void camera_store_write_default_file(void)
{
    FILE *f = fopen(APP_CAMERAS_PATH, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cannot create default cameras file %s", APP_CAMERAS_PATH);
        return;
    }
    const char empty[] = "[]";
    fwrite(empty, 1U, sizeof(empty) - 1U, f);
    fclose(f);
}

static bool camera_store_copy_str(const cJSON *obj, const char *key, char *dst, size_t dst_size)
{
    const char *value = NULL;
    if (!json_util_get_string(obj, key, &value) || value == NULL) {
        dst[0] = '\0';
        return false;
    }
    snprintf(dst, dst_size, "%s", value);
    return true;
}

static uint32_t camera_store_clamp_refresh(int value)
{
    if (value < (int)APP_CAMERA_REFRESH_MIN_MS) return APP_CAMERA_REFRESH_MIN_MS;
    if (value > (int)APP_CAMERA_REFRESH_MAX_MS) return APP_CAMERA_REFRESH_MAX_MS;
    return (uint32_t)value;
}

static void camera_store_generate_id(size_t index, char *dst, size_t dst_size)
{
    snprintf(dst, dst_size, "cam_%u", (unsigned)(index + 1U));
}

/* Older firmware exported the camera list inside a section envelope such as
 * {"value":[...],"Count":2}.  Such a file is still present on some panels; without
 * unwrapping it every entry looks invalid, which is what left the cameras page
 * claiming there were no cameras at all. */
static cJSON *camera_store_envelope_list(cJSON *obj)
{
    static const char *keys[] = {"value", "cameras", "items", "entries", "list"};

    if (!cJSON_IsObject(obj)) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cJSON *inner = cJSON_GetObjectItemCaseSensitive(obj, keys[i]);
        if (cJSON_IsArray(inner)) {
            return inner;
        }
    }
    return NULL;
}

static cJSON *camera_store_list_root(cJSON *root)
{
    cJSON *node = root;

    for (int depth = 0; depth < 4 && node != NULL; depth++) {
        cJSON *inner = NULL;
        if (cJSON_IsArray(node)) {
            if (cJSON_GetArraySize(node) != 1) {
                return node;
            }
            inner = camera_store_envelope_list(cJSON_GetArrayItem(node, 0));
        } else {
            inner = camera_store_envelope_list(node);
        }
        if (inner == NULL) {
            return cJSON_IsArray(node) ? node : NULL;
        }
        node = inner;
    }
    return NULL;
}

static esp_err_t camera_store_parse_json(const char *json,
                                         camera_entry_t *entries,
                                         size_t *count_out)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    cJSON *list = camera_store_list_root(root);
    if (list == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    size_t count = 0;
    size_t candidates = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, list) {
        if (count >= APP_MAX_CAMERAS) {
            break;
        }
        candidates++;
        if (!cJSON_IsObject(item)) {
            ESP_LOGW(TAG, "ignoring camera entry that is not an object");
            continue;
        }

        camera_entry_t entry;
        memset(&entry, 0, sizeof(entry));

        /* Source defaults to "http" so legacy entries (URL-only) keep working. */
        camera_store_copy_str(item, "source", entry.source, sizeof(entry.source));
        if (entry.source[0] == '\0') {
            snprintf(entry.source, sizeof(entry.source), "http");
        }
        if (strcmp(entry.source, "ha") != 0) {
            snprintf(entry.source, sizeof(entry.source), "http");
        }

        camera_store_copy_str(item, "entity_id", entry.entity_id, sizeof(entry.entity_id));
        camera_store_copy_str(item, "snapshot_url", entry.snapshot_url, sizeof(entry.snapshot_url));

        if (strcmp(entry.source, "ha") == 0) {
            if (entry.entity_id[0] == '\0') {
                ESP_LOGW(TAG, "ignoring HA camera without entity_id");
                continue;
            }
            if (strncmp(entry.entity_id, "camera.", 7) != 0) {
                ESP_LOGW(TAG, "ignoring HA camera with non-camera entity_id");
                continue;
            }
        } else {
            if (entry.snapshot_url[0] == '\0') {
                ESP_LOGW(TAG, "ignoring camera without snapshot_url");
                continue; /* URL is mandatory for http source */
            }
            if (strncmp(entry.snapshot_url, "http://", 7) != 0 &&
                strncmp(entry.snapshot_url, "https://", 8) != 0) {
                ESP_LOGW(TAG, "ignoring camera with non-absolute URL");
                continue;
            }
        }

        camera_store_copy_str(item, "id", entry.id, sizeof(entry.id));
        if (entry.id[0] == '\0') {
            camera_store_generate_id(count, entry.id, sizeof(entry.id));
        }
        camera_store_copy_str(item, "name", entry.name, sizeof(entry.name));
        if (entry.name[0] == '\0') {
            if (strcmp(entry.source, "ha") == 0 && entry.entity_id[0] != '\0') {
                strncpy(entry.name, entry.entity_id, sizeof(entry.name) - 1);
                entry.name[sizeof(entry.name) - 1] = '\0';
            } else {
                snprintf(entry.name, sizeof(entry.name), "Kamera %u", (unsigned)(count + 1U));
            }
        }
        camera_store_copy_str(item, "username", entry.username, sizeof(entry.username));
        camera_store_copy_str(item, "password", entry.password, sizeof(entry.password));

        int refresh = APP_CAMERA_REFRESH_DEFAULT_MS;
        json_util_get_int(item, "refresh_ms", &refresh);
        entry.refresh_ms = camera_store_clamp_refresh(refresh);

        cJSON *enabled_json = cJSON_GetObjectItem(item, "enabled");
        entry.enabled = (enabled_json == NULL) ? true : cJSON_IsTrue(enabled_json);

        if (entries != NULL) {
            entries[count] = entry;
        }
        count++;
    }

    cJSON_Delete(root);

    /* A payload whose every entry got rejected is a mistake (legacy envelope,
     * template body, missing URL): refuse it instead of silently storing a list
     * that would leave the cameras page empty. */
    if (ret == ESP_OK && count == 0 && candidates > 0) {
        ESP_LOGW(TAG, "%u camera entries rejected: payload holds no usable camera",
                 (unsigned)candidates);
        ret = ESP_ERR_INVALID_ARG;
    }
    if (count_out != NULL) {
        *count_out = (ret == ESP_OK) ? count : 0;
    }
    return ret;
}

static esp_err_t camera_store_read_file(char **out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    FILE *f = fopen(APP_CAMERAS_PATH, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size <= 0 || size > APP_CAMERAS_MAX_JSON_LEN) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    char *buf = (char *)calloc((size_t)size + 1U, sizeof(char));
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read = fread(buf, 1U, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }
    *out = buf;
    return ESP_OK;
}

static char *camera_store_serialize_entries(const camera_entry_t *entries, size_t count)
{
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddStringToObject(obj, "id", entries[i].id);
        cJSON_AddStringToObject(obj, "name", entries[i].name);
        cJSON_AddStringToObject(obj, "source", entries[i].source);
        cJSON_AddStringToObject(obj, "entity_id", entries[i].entity_id);
        cJSON_AddStringToObject(obj, "snapshot_url", entries[i].snapshot_url);
        cJSON_AddStringToObject(obj, "username", entries[i].username);
        cJSON_AddStringToObject(obj, "password", entries[i].password);
        cJSON_AddNumberToObject(obj, "refresh_ms", (double)entries[i].refresh_ms);
        cJSON_AddBoolToObject(obj, "enabled", entries[i].enabled);
        cJSON_AddItemToArray(array, obj);
    }

    char *out = json_util_print_unformatted(array);
    cJSON_Delete(array);
    return out;
}

esp_err_t camera_store_load(camera_entry_t *entries, size_t *count_out)
{
    if (count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count_out = 0;

    char *buf = NULL;
    esp_err_t err = camera_store_read_file(&buf);
    if (err != ESP_OK) {
        return err;
    }

    err = camera_store_parse_json(buf, entries, count_out);
    free(buf);
    return err;
}

esp_err_t camera_store_init(void)
{
    char *raw = NULL;
    esp_err_t err = camera_store_read_file(&raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cameras file missing, writing empty list");
        camera_store_write_default_file();
        return ESP_OK;
    }

    camera_entry_t *entries = (camera_entry_t *)calloc(APP_MAX_CAMERAS, sizeof(camera_entry_t));
    if (entries == NULL) {
        free(raw);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    err = camera_store_parse_json(raw, entries, &count);
    char *normalized = (err == ESP_OK) ? camera_store_serialize_entries(entries, count) : NULL;
    free(entries);

    /* Files written by older firmware used an envelope and escaped payloads;
     * rewrite them once in the canonical shape so every reader agrees. */
    if (normalized != NULL && strcmp(normalized, raw) != 0) {
        FILE *f = fopen(APP_CAMERAS_PATH, "wb");
        if (f != NULL) {
            size_t len = strlen(normalized);
            bool ok = (fwrite(normalized, 1U, len, f) == len);
            fclose(f);
            if (ok) {
                ESP_LOGI(TAG, "cameras file normalised (%u cameras)", (unsigned)count);
            } else {
                ESP_LOGW(TAG, "cameras file rewrite failed");
            }
        }
    } else if (normalized == NULL) {
        ESP_LOGW(TAG, "cameras file unusable (%s), leaving it untouched", esp_err_to_name(err));
    }

    free(normalized);
    free(raw);
    return ESP_OK;
}

esp_err_t camera_store_save_json(const char *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate by parsing; never persist an unparsable payload. */
    esp_err_t err = camera_store_parse_json(json, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    FILE *f = fopen(APP_CAMERAS_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "cannot open cameras file for writing: %s", APP_CAMERAS_PATH);
        return ESP_FAIL;
    }
    size_t len = strlen(json);
    size_t written = fwrite(json, 1U, len, f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "failed to write cameras file");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved cameras (%u bytes)", (unsigned)len);
    return ESP_OK;
}

esp_err_t camera_store_load_json(char **json_out)
{
    if (json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;

    char *buf = NULL;
    esp_err_t err = camera_store_read_file(&buf);
    if (err != ESP_OK) {
        return err;
    }

    /* Always hand out a plain array: a client that receives a legacy envelope
     * would show it as a camera and write it straight back on the next save. */
    camera_entry_t *entries = (camera_entry_t *)calloc(APP_MAX_CAMERAS, sizeof(camera_entry_t));
    if (entries == NULL) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    err = camera_store_parse_json(buf, entries, &count);
    free(buf);
    if (err != ESP_OK) {
        free(entries);
        return err;
    }

    char *out = camera_store_serialize_entries(entries, count);
    free(entries);
    if (out == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *json_out = out;
    return ESP_OK;
}
