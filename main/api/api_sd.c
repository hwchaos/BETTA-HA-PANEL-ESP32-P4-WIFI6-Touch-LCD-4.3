/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "app_config.h"
#include "sd/sd_card.h"
#include "sd/sd_logs.h"
#include "ui/ui_screen_saver.h"
#include "util/log_tags.h"

#define SD_BODY_MAX_LEN 512
#define SD_STREAM_CHUNK_BYTES 4096

static const char *TAG = TAG_SD;

static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status)
{
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

/* Short machine-readable state for the web UI: "ok" / "off" / "no_card" /
 * "exfat" / "ntfs" / "no_filesystem" / "unsupported".  The filesystem variants
 * let the web UI tell the user what to do about a card it cannot mount. */
static const char *sd_state_name(const sd_card_info_t *info)
{
    if (!info->supported) {
        return "unsupported";
    }
    if (!info->enabled) {
        return "off";
    }
    if (info->mounted) {
        return "ok";
    }
    if (!info->detected) {
        return "no_card";
    }
    if (strcasecmp(info->fs_name, "exFAT") == 0) {
        return "exfat";
    }
    if (strcasecmp(info->fs_name, "NTFS") == 0) {
        return "ntfs";
    }
    return "no_filesystem";
}

static cJSON *build_status(void)
{
    sd_card_info_t info;
    if (sd_card_get_info(&info) != ESP_OK) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    const char *state = sd_state_name(&info);

    cJSON_AddBoolToObject(root, "supported", info.supported);
    cJSON_AddBoolToObject(root, "enabled", info.enabled);
    cJSON_AddBoolToObject(root, "mounted", info.mounted);
    cJSON_AddBoolToObject(root, "detected", info.detected);
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "error", strcmp(state, "ok") == 0 ? "" : state);
    cJSON_AddStringToObject(root, "card_name", info.card_name);
    cJSON_AddStringToObject(root, "fs_name", info.fs_name);
    cJSON_AddNumberToObject(root, "capacity_bytes", (double)info.capacity_bytes);
    cJSON_AddNumberToObject(root, "free_bytes", (double)info.free_bytes);
    cJSON_AddNumberToObject(root, "sector_bytes", (double)info.sector_bytes);
    cJSON_AddNumberToObject(root, "mount_error", (double)info.mount_error);
    cJSON_AddNumberToObject(root, "used_bytes", (double)(info.capacity_bytes > info.free_bytes
                                                             ? info.capacity_bytes - info.free_bytes
                                                             : 0));
    cJSON_AddStringToObject(root, "log_dir", APP_SD_LOG_DIR);
    cJSON_AddStringToObject(root, "photo_dir", APP_SD_PHOTO_DIR);
    /* Where the screensaver picture is kept: the web UI shows this next to the
     * card state ("wallpaper_store": "sd" while the frame is on the card).
     * "wallpaper_on_card" answers "is the card the active store?", not "does a
     * frame exist on the card?" - ask "wallpaper_present" (or read
     * "wallpaper_store") before claiming that a picture is stored. */
    cJSON_AddBoolToObject(root, "wallpaper_present", ui_screen_saver_wallpaper_present());
    cJSON_AddBoolToObject(root, "wallpaper_on_card", ui_screen_saver_wallpaper_on_card());
    cJSON_AddStringToObject(root, "wallpaper_store",
                            ui_screen_saver_wallpaper_present()
                                ? (ui_screen_saver_wallpaper_on_card() ? "sd" : "flash")
                                : "none");
    cJSON_AddNumberToObject(root, "log_max_files", APP_SD_LOG_MAX_FILES);
    cJSON_AddNumberToObject(root, "log_max_bytes", APP_SD_LOG_MAX_BYTES);
    return root;
}

/* Reads the "path"/"dir" query parameter and URL-decodes it in place. */
static bool query_get_path(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 2U * APP_SD_MAX_PATH_LEN) {
        return false;
    }

    char *qbuf = calloc(qlen + 1U, sizeof(char));
    if (qbuf == NULL) {
        return false;
    }
    if (httpd_req_get_url_query_str(req, qbuf, qlen + 1U) != ESP_OK) {
        free(qbuf);
        return false;
    }

    esp_err_t err = httpd_query_key_value(qbuf, key, out, out_len);
    free(qbuf);
    if (err != ESP_OK) {
        return false;
    }

    /* Minimal percent-decoding: file names coming from the browser are escaped. */
    char *read = out;
    char *write = out;
    while (*read != '\0') {
        if (read[0] == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2])) {
            char hex[3] = {read[1], read[2], '\0'};
            *write++ = (char)strtol(hex, NULL, 16);
            read += 3;
        } else if (read[0] == '+') {
            *write++ = ' ';
            read++;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
    return out[0] != '\0';
}

static const char *content_type_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".log") == 0 || strcasecmp(dot, ".txt") == 0) {
        return "text/plain";
    }
    if (strcasecmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".bmp") == 0) {
        return "image/bmp";
    }
    if (strcasecmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(dot, ".webp") == 0) {
        return "image/webp";
    }
    return "application/octet-stream";
}

esp_err_t api_sd_status_get_handler(httpd_req_t *req)
{
    return send_json(req, build_status(), NULL);
}

esp_err_t api_sd_status_put_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > SD_BODY_MAX_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, cJSON_CreateObject(), NULL);
    }

    char body[SD_BODY_MAX_LEN + 1];
    size_t total = 0;
    while (total < req->content_len) {
        int got = httpd_req_recv(req, body + total, req->content_len - total);
        if (got <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return send_json(req, cJSON_CreateObject(), NULL);
        }
        total += (size_t)got;
    }
    body[total] = '\0';

    cJSON *parsed = cJSON_Parse(body);
    if (parsed == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, cJSON_CreateObject(), NULL);
    }

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(parsed, "enabled");
    bool requested = cJSON_IsTrue(enabled);
    cJSON_Delete(parsed);

    esp_err_t err = sd_card_set_enabled(requested);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "Cannot apply microSD enabled=%d (%s)", (int)requested, esp_err_to_name(err));
    }

    const char *status = (err == ESP_OK) ? NULL : "409 Conflict";
    return send_json(req, build_status(), status);
}

esp_err_t api_sd_format_post_handler(httpd_req_t *req)
{
    esp_err_t err = sd_card_format();
    if (err == ESP_ERR_INVALID_STATE) {
        /* A second request while the first format is still running (double
         * click, two tabs) must not start another destructive pass. */
        ESP_LOGW(TAG, "Format already in progress; second request rejected");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Format request failed: %s", esp_err_to_name(err));
    }

    const char *status = (err == ESP_OK) ? NULL : "409 Conflict";
    return send_json(req, build_status(), status);
}

esp_err_t api_sd_files_get_handler(httpd_req_t *req)
{
    char dir[APP_SD_MAX_PATH_LEN];
    if (!query_get_path(req, "dir", dir, sizeof(dir))) {
        strlcpy(dir, "", sizeof(dir));
    }

    if (!sd_card_is_mounted()) {
        return send_json(req, build_status(), "409 Conflict");
    }

    sd_dir_entry_t *entries = calloc(APP_SD_MAX_FILES, sizeof(sd_dir_entry_t));
    if (entries == NULL) {
        return httpd_resp_send_500(req);
    }

    size_t count = 0;
    esp_err_t err = sd_card_list(dir, entries, APP_SD_MAX_FILES, &count);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        free(entries);
        return send_json(req, build_status(), "409 Conflict");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        free(entries);
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(root, "dir", dir);
    cJSON_AddBoolToObject(root, "truncated", err == ESP_ERR_NOT_FOUND ? false : count >= APP_SD_MAX_FILES);

    cJSON *list = cJSON_AddArrayToObject(root, "entries");
    for (size_t i = 0; list != NULL && i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            break;
        }
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddNumberToObject(item, "size", (double)entries[i].size);
        cJSON_AddBoolToObject(item, "dir", entries[i].is_dir);
        cJSON_AddItemToArray(list, item);
    }
    free(entries);

    return send_json(req, root, NULL);
}

esp_err_t api_sd_file_get_handler(httpd_req_t *req)
{
    char rel[APP_SD_MAX_PATH_LEN];
    if (!query_get_path(req, "path", rel, sizeof(rel))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing path");
    }

    char path[APP_SD_MAX_PATH_LEN];
    if (!sd_card_build_path(rel, path, sizeof(path))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid path");
    }

    struct stat st;
    if (!sd_card_is_mounted() || stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "not found");
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "not found");
    }

    char *buf = malloc(SD_STREAM_CHUNK_BYTES);
    if (buf == NULL) {
        fclose(file);
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, content_type_for(rel));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t err = ESP_OK;
    size_t got = 0;
    while ((got = fread(buf, 1, SD_STREAM_CHUNK_BYTES, file)) > 0) {
        err = httpd_resp_send_chunk(req, buf, got);
        if (err != ESP_OK) {
            break;
        }
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }

    free(buf);
    fclose(file);
    return err;
}

esp_err_t api_sd_file_delete_handler(httpd_req_t *req)
{
    char rel[APP_SD_MAX_PATH_LEN];
    if (!query_get_path(req, "path", rel, sizeof(rel))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing path");
    }

    esp_err_t err = sd_card_remove(rel);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "not found");
    }

    /* Deleting the frame from the card also drops the copy the panel holds in
     * RAM, otherwise the picture would stay on the screen while the card looks
     * empty and would vanish at the next restart. */
    if (strcmp(rel, APP_SD_PHOTO_DIR "/wallpaper.bin") == 0) {
        ui_screen_saver_reload_wallpaper();
    }

    return send_json(req, build_status(), NULL);
}

esp_err_t api_sd_logs_export_post_handler(httpd_req_t *req)
{
    char name[APP_SD_MAX_NAME_LEN] = {0};
    esp_err_t err = sd_logs_export(name, sizeof(name));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Log export failed: %s", esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "file", name);
    cJSON_AddStringToObject(root, "dir", APP_SD_LOG_DIR);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }

    return send_json(req, root, err == ESP_OK ? NULL : "409 Conflict");
}
