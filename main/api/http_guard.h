/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef esp_err_t (*http_guard_handler_t)(httpd_req_t *req);

esp_err_t http_guard_init(void);
esp_err_t http_guard_handle(httpd_req_t *req, http_guard_handler_t next_handler);

/* Optional password for every page and API call (HTTP Basic auth, any user
 * name). NULL or "" switches the protection off. */
void http_guard_set_password(const char *password);
bool http_guard_password_set(void);
