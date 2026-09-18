/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "settings/runtime_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decides which theme is painted right now.
 *
 * Resolution order for the visible page:
 *   1. the page's own "page_theme" key from the layout JSON,
 *   2. the day/night schedule theme (when the automatic mode is enabled and the
 *      night window is known from the synced clock),
 *   3. the global theme the user picked (theme store / /api/themes/active).
 *
 * The router never touches LVGL from a foreign task: a page switch only records
 * what should be painted, and the palette swap plus the layout rebuild happen in
 * ui_theme_router_tick(), which runs on the UI task. */

/* Caches the active palette as the global (base) theme. Call after
 * theme_store_init() has re-activated the persisted theme. */
void ui_theme_router_init(void);

/* The global theme changed (web UI / REST). Must not be called with a page
 * override in mind: the override, when present, still wins. */
void ui_theme_router_set_base_id(const char *theme_id);

/* Global theme id, "dark_v2" when nothing was set. */
const char *ui_theme_router_base_id(void);

/* Theme currently painted on screen ("" before the first apply). */
const char *ui_theme_router_applied_id(void);

/* Caches the automatic day/night schedule. Safe to call from any task. */
void ui_theme_router_apply_settings(const runtime_settings_t *settings);

/* Page overrides, filled while the layout JSON is parsed. */
void ui_theme_router_reset_pages(void);
void ui_theme_router_set_page_theme(const char *page_id, const char *theme_id);

/* Theme configured for a page, "" when the page has no override. */
const char *ui_theme_router_page_theme(const char *page_id);

/* Drops every page override that points at theme_id (called when a custom theme
 * is deleted) so the pages fall back to the global/daily theme instead of
 * showing a stale id. Safe to call from any task. */
void ui_theme_router_forget_theme(const char *theme_id);

/* Registers the page that is now visible (call it from the page shown
 * callback). Cheap: no LVGL, no file system. */
void ui_theme_router_notify_page_shown(const char *page_id);

/* Runs on the UI task from the ui_runtime loop: applies a queued page override
 * or the day/night transition by activating the palette and rebuilding the
 * layout on the page that is currently visible. */
void ui_theme_router_tick(void);

/* Diagnostics: how a page resolves right now. "" is never returned, the global
 * theme is used as the last resort. */
const char *ui_theme_router_effective_id(void);

#ifdef __cplusplus
}
#endif
