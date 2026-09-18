/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#include "sd/sd_card.h"
#include "settings/runtime_settings.h"

void ui_screen_saver_init(void);
void ui_screen_saver_apply_settings(const runtime_settings_t *settings);

/* True while the configured night schedule is forcing the night backlight
 * level. A touch grants a temporary wake window inside that schedule. */
bool ui_screen_saver_night_active(void);

/* Reveal counters. A reveal is "the clock was put on the glass"; a blip is a
 * reveal that a wake dismissed before the light wallpaper had faded in - the
 * blink reported as a light-blue screen flash. Any of the pointers may be NULL. */
void ui_screen_saver_get_reveal_stats(uint32_t *reveals, uint32_t *blips, int64_t *last_ms);

/* Wake the panel to the screensaver clock (used by MQTT/HA "wake" command and
 * motion sensors). A real touch dismisses the clock and opens the full UI. */
void ui_screen_saver_wake(void);

/* Reads the wallpaper frame from its storage before the UI takes the display
 * lock, so the 1.2 MB read cannot stall the LVGL task during startup.  The
 * frame is handed to the UI by ui_screen_saver_init(). */
void ui_screen_saver_wallpaper_preload(void);

/* Reload the screensaver wallpaper from its storage (called after upload,
 * delete, or a microSD change). */
void ui_screen_saver_reload_wallpaper(void);

/* Where the frame is stored: the microSD card while one is mounted, internal
 * LittleFS otherwise.  Only one copy is kept, so uploads, reads and deletes go
 * to ui_screen_saver_wallpaper_path(). */
const char *ui_screen_saver_wallpaper_path(void);
const char *ui_screen_saver_wallpaper_tmp_path(void);
bool ui_screen_saver_wallpaper_on_card(void);
bool ui_screen_saver_wallpaper_present(void);

/* Keeps that single copy in step with the card: moves the frame onto a card
 * that appeared and writes it back to internal flash right before the card is
 * released.  Registered as the microSD event callback. */
void ui_screen_saver_wallpaper_sync(sd_card_event_t event);

/* The loaded wallpaper frame (native panel resolution RGB565) or NULL when no
 * wallpaper is installed. Pages that paint the wallpaper crop the content box
 * out of this frame. */
const lv_image_dsc_t *ui_screen_saver_wallpaper_dsc(void);

/* Must be called by ui_pages_init() right before it cleans the active screen:
 * the clock overlay is a child of that screen and is destroyed by the clean-up,
 * so its handles have to be dropped or the tick timer would keep writing into
 * freed LVGL objects (heap corruption / frozen panel). */
void ui_screen_saver_handle_screen_clean(void);
