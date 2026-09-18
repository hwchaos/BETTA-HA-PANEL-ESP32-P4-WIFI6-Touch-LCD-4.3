/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Manual camera configuration store.  Cameras are optional, user-entered
 * entries persisted as a JSON array at APP_CAMERAS_PATH (/littlefs/cameras.json).
 * Each entry points at an HTTP JPEG snapshot URL (Hikvision /ISAPI/.../picture,
 * Dahua /cgi-bin/snapshot.cgi, ...) plus optional basic-auth credentials.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_config.h"

typedef struct {
    char id[APP_CAMERA_ID_MAX_LEN];
    char name[APP_CAMERA_NAME_MAX_LEN];
    char source[APP_CAMERA_SOURCE_MAX_LEN];   /* "ha" or "http" */
    char entity_id[APP_CAMERA_ENTITY_MAX_LEN];/* HA camera entity for source=="ha" */
    char snapshot_url[APP_CAMERA_URL_MAX_LEN];
    char username[APP_CAMERA_USER_MAX_LEN];
    char password[APP_CAMERA_PASS_MAX_LEN];
    uint32_t refresh_ms;
    bool enabled;
} camera_entry_t;

/* Ensures the file exists (writing an empty list on first boot).  Files saved in
 * a legacy shape (envelope keys such as {"value":[…],"Count":n}) are rewritten in
 * the canonical array form, so the cameras page always sees the stored cameras. */
esp_err_t camera_store_init(void);

/* Loads up to *count_out entries into `entries` (caller provides an array of
 * APP_MAX_CAMERAS). Returns the number of valid entries in *count_out. */
esp_err_t camera_store_load(camera_entry_t *entries, size_t *count_out);

/* Persists a JSON payload after validating it: a body whose entries are all
 * invalid (envelope, missing URL/entity) is rejected instead of emptying the
 * camera list. */
esp_err_t camera_store_save_json(const char *json);

/* Serializes the current on-disk list to a freshly allocated JSON array (legacy
 * envelopes are unwrapped, invalid entries dropped). Caller frees with free(). */
esp_err_t camera_store_load_json(char **json_out);
