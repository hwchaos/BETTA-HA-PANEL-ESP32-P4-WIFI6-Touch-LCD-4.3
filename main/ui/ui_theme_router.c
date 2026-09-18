/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_theme_router.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ui/theme/theme_palette.h"
#include "ui/ui_runtime.h"
#include "util/log_tags.h"

#define TAG_THEME_ROUTER "theme_router"

#ifndef APP_MAX_THEME_PAGES
#define APP_MAX_THEME_PAGES 12
#endif

/* The schedule only needs a minute resolution, so a one second poll is plenty. */
#define THEME_SCHEDULE_POLL_MS 1000

typedef struct {
    char page_id[APP_MAX_PAGE_ID_LEN];
    char theme_id[APP_MAX_THEME_ID_LEN];
} page_theme_slot_t;

static page_theme_slot_t s_pages[APP_MAX_THEME_PAGES];
static size_t s_page_count;

static char s_base_id[APP_MAX_THEME_ID_LEN];
static char s_applied_id[APP_MAX_THEME_ID_LEN];
static char s_current_page[APP_MAX_PAGE_ID_LEN];
static char s_pending_theme[APP_MAX_THEME_ID_LEN];

static char s_day_id[APP_MAX_THEME_ID_LEN];
static char s_night_id[APP_MAX_THEME_ID_LEN];
static uint16_t s_night_start_min;
static uint16_t s_night_end_min;
static bool s_auto_enabled;
static int64_t s_last_schedule_ms;
static bool s_initialised;
/* Id that failed to load, so a broken page override or a deleted custom theme is
 * not retried on every schedule poll. Cleared whenever the settings change. */
static char s_failed_id[APP_MAX_THEME_ID_LEN];

static bool theme_router_is_known(const char *theme_id)
{
    return theme_id != NULL && theme_id[0] != '\0' && theme_palette_find_builtin(theme_id) != NULL;
}

/* Minutes since midnight, -1 while the clock is not synced yet. */
static int theme_router_minutes_of_day(void)
{
    time_t now = time(NULL);
    struct tm info = {0};
    localtime_r(&now, &info);
    if (info.tm_year < 120) {
        return -1;
    }
    return (info.tm_hour * 60) + info.tm_min;
}

/* Same window semantics as the screen saver: [start, end), may cross midnight.
 * The theme switch only needs the times, the night brightness stays optional. */
static bool theme_router_night_window_active(void)
{
    int minutes = theme_router_minutes_of_day();
    if (minutes < 0 || s_night_start_min == s_night_end_min) {
        return false;
    }
    if (s_night_start_min < s_night_end_min) {
        return minutes >= (int)s_night_start_min && minutes < (int)s_night_end_min;
    }
    return minutes >= (int)s_night_start_min || minutes < (int)s_night_end_min;
}

void ui_theme_router_init(void)
{
    memset(s_pages, 0, sizeof(s_pages));
    s_page_count = 0;
    s_pending_theme[0] = '\0';
    s_current_page[0] = '\0';
    s_day_id[0] = '\0';
    s_night_id[0] = '\0';
    s_auto_enabled = false;
    s_last_schedule_ms = 0;
    s_night_start_min = 0;
    s_night_end_min = 0;
    s_failed_id[0] = '\0';

    const char *active = theme_palette_active_id();
    snprintf(s_applied_id, sizeof(s_applied_id), "%s", (active != NULL && active[0] != '\0') ? active : "dark_v2");
    snprintf(s_base_id, sizeof(s_base_id), "%s", s_applied_id);
    s_initialised = true;
    ESP_LOGI(TAG_THEME_ROUTER, "theme router ready (global theme %s)", s_base_id);
}

void ui_theme_router_set_base_id(const char *theme_id)
{
    if (theme_id == NULL || theme_id[0] == '\0') {
        return;
    }
    if (strncmp(s_base_id, theme_id, sizeof(s_base_id)) == 0) {
        return;
    }
    snprintf(s_base_id, sizeof(s_base_id), "%s", theme_id);
    /* The caller already activated that palette; treat it as painted so the next
     * tick does not rebuild the layout a second time. */
    snprintf(s_applied_id, sizeof(s_applied_id), "%s", theme_id);
    s_pending_theme[0] = '\0';
    s_failed_id[0] = '\0';
    /* With the automatic mode on, a manual pick wins for the current half of the
     * day (otherwise the schedule would undo it a second later). */
    if (s_auto_enabled) {
        char *slot = theme_router_night_window_active() ? s_night_id : s_day_id;
        snprintf(slot, APP_MAX_THEME_ID_LEN, "%s", theme_id);
    }
}

const char *ui_theme_router_base_id(void)
{
    return s_base_id[0] != '\0' ? s_base_id : "dark_v2";
}

const char *ui_theme_router_applied_id(void)
{
    return s_applied_id;
}

void ui_theme_router_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }
    bool was_auto = s_auto_enabled;
    char prev_day[APP_MAX_THEME_ID_LEN];
    char prev_night[APP_MAX_THEME_ID_LEN];
    snprintf(prev_day, sizeof(prev_day), "%s", s_day_id);
    snprintf(prev_night, sizeof(prev_night), "%s", s_night_id);

    s_auto_enabled = settings->display_theme_auto_enabled;
    snprintf(s_day_id, sizeof(s_day_id), "%s", settings->display_theme_day_id);
    snprintf(s_night_id, sizeof(s_night_id), "%s", settings->display_theme_night_id);
    s_night_start_min = settings->display_night_start_min;
    s_night_end_min = settings->display_night_end_min;
    s_last_schedule_ms = 0;
    s_failed_id[0] = '\0';

    if (was_auto != s_auto_enabled || strcmp(prev_day, s_day_id) != 0 || strcmp(prev_night, s_night_id) != 0) {
        ESP_LOGI(TAG_THEME_ROUTER, "auto theme %s (day=%s night=%s)", s_auto_enabled ? "on" : "off",
                 s_day_id[0] != '\0' ? s_day_id : "-", s_night_id[0] != '\0' ? s_night_id : "-");
    }
}

void ui_theme_router_reset_pages(void)
{
    memset(s_pages, 0, sizeof(s_pages));
    s_page_count = 0;
}

void ui_theme_router_set_page_theme(const char *page_id, const char *theme_id)
{
    if (page_id == NULL || page_id[0] == '\0') {
        return;
    }
    if (theme_id == NULL || theme_id[0] == '\0') {
        return;
    }
    if (s_page_count >= APP_MAX_THEME_PAGES) {
        ESP_LOGW(TAG_THEME_ROUTER, "page theme table full, ignoring %s", page_id);
        return;
    }
    if (!theme_router_is_known(theme_id)) {
        /* Custom themes are looked up on demand; log it so a typo is visible. */
        ESP_LOGI(TAG_THEME_ROUTER, "page %s wants non built-in theme '%s'", page_id, theme_id);
    }
    snprintf(s_pages[s_page_count].page_id, sizeof(s_pages[s_page_count].page_id), "%s", page_id);
    snprintf(s_pages[s_page_count].theme_id, sizeof(s_pages[s_page_count].theme_id), "%s", theme_id);
    s_page_count++;
}

const char *ui_theme_router_page_theme(const char *page_id)
{
    if (page_id == NULL || page_id[0] == '\0') {
        return "";
    }
    for (size_t i = 0; i < s_page_count; i++) {
        if (strcmp(s_pages[i].page_id, page_id) == 0) {
            return s_pages[i].theme_id;
        }
    }
    return "";
}

/* Page override, then the schedule, then the global theme. Callers must treat
 * the result as read-only. */
static const char *theme_router_effective_id(void)
{
    const char *override_id = ui_theme_router_page_theme(s_current_page);
    if (override_id[0] != '\0') {
        return override_id;
    }
    if (s_auto_enabled) {
        const char *wanted = theme_router_night_window_active() ? s_night_id : s_day_id;
        if (wanted[0] != '\0') {
            return wanted;
        }
    }
    return ui_theme_router_base_id();
}

const char *ui_theme_router_effective_id(void)
{
    return theme_router_effective_id();
}

void ui_theme_router_forget_theme(const char *theme_id)
{
    if (theme_id == NULL || theme_id[0] == '\0') {
        return;
    }
    for (size_t i = 0; i < s_page_count; i++) {
        if (strcmp(s_pages[i].theme_id, theme_id) == 0) {
            ESP_LOGI(TAG_THEME_ROUTER, "page %s override %s gone, using the global theme", s_pages[i].page_id,
                     theme_id);
            s_pages[i].theme_id[0] = '\0';
        }
    }
    if (strcmp(s_day_id, theme_id) == 0) {
        s_day_id[0] = '\0';
    }
    if (strcmp(s_night_id, theme_id) == 0) {
        s_night_id[0] = '\0';
    }
    if (strcmp(s_pending_theme, theme_id) == 0) {
        s_pending_theme[0] = '\0';
    }
    if (strcmp(s_failed_id, theme_id) == 0) {
        s_failed_id[0] = '\0';
    }
}

void ui_theme_router_notify_page_shown(const char *page_id)
{
    if (!s_initialised) {
        return;
    }
    snprintf(s_current_page, sizeof(s_current_page), "%s", page_id != NULL ? page_id : "");

    /* No file system and no LVGL access here: only record what should be
     * painted. The palette swap and the layout rebuild happen in the tick, so
     * the pages are never torn down from inside the show callback. */
    const char *wanted = theme_router_effective_id();
    if (wanted == NULL || wanted[0] == '\0' || strcmp(wanted, s_applied_id) == 0) {
        return;
    }
    if (s_failed_id[0] != '\0' && strcmp(wanted, s_failed_id) == 0) {
        return;
    }
    snprintf(s_pending_theme, sizeof(s_pending_theme), "%s", wanted);
}

void ui_theme_router_tick(void)
{
    if (!s_initialised) {
        return;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_pending_theme[0] == '\0' && s_auto_enabled &&
        (s_last_schedule_ms == 0 || (now_ms - s_last_schedule_ms) >= THEME_SCHEDULE_POLL_MS)) {
        s_last_schedule_ms = now_ms;
        const char *wanted = theme_router_effective_id();
        if (wanted != NULL && wanted[0] != '\0' && strcmp(wanted, s_applied_id) != 0 &&
            !(s_failed_id[0] != '\0' && strcmp(wanted, s_failed_id) == 0)) {
            snprintf(s_pending_theme, sizeof(s_pending_theme), "%s", wanted);
        }
    }

    if (s_pending_theme[0] == '\0') {
        return;
    }

    char wanted[APP_MAX_THEME_ID_LEN];
    snprintf(wanted, sizeof(wanted), "%s", s_pending_theme);
    s_pending_theme[0] = '\0';

    if (strcmp(wanted, s_applied_id) == 0) {
        return;
    }

    esp_err_t err = theme_palette_activate_by_id(wanted);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_THEME_ROUTER, "theme '%s' unavailable (%s), using %s", wanted, esp_err_to_name(err),
                 ui_theme_router_base_id());
        /* Do not retry this id on every poll; it either comes back with a new
         * settings push or a theme save. */
        snprintf(s_failed_id, sizeof(s_failed_id), "%s", wanted);
        snprintf(wanted, sizeof(wanted), "%s", ui_theme_router_base_id());
        if (strcmp(wanted, s_applied_id) == 0 || theme_palette_activate_by_id(wanted) != ESP_OK) {
            return;
        }
    }

    snprintf(s_applied_id, sizeof(s_applied_id), "%s", wanted);
    ESP_LOGI(TAG_THEME_ROUTER, "applying theme %s on page %s", s_applied_id,
             s_current_page[0] != '\0' ? s_current_page : "-");
    /* Rebuild keeps the visible page instead of jumping back to the first one. */
    (void)ui_runtime_request_layout_reload_on_page(s_current_page[0] != '\0' ? s_current_page : NULL);
}
