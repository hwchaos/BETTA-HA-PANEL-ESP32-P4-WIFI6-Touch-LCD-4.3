/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_pages.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "diag/display_flash_watch.h"
#include "diag/system_log.h"
#include "drivers/display_init.h"
#include "esp_log.h"
#include "settings/runtime_settings.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/ui_i18n.h"
#include "ui/ui_assist.h"
#include "ui/ui_geometry.h"
#include "ui/ui_topbar_waste.h"
#include "ui/ui_topbar_alerts.h"
#include "ui/ui_mdi_icon.h"
#include "ha/ha_client.h"
#include "assist/panel_assist.h"
#include "ui/theme/theme_fx.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_page_transition.h"
#include "ui/ui_screen_saver.h"
#include "ui/ui_settings.h"
#include "util/log_tags.h"

typedef struct {
    char id[APP_MAX_PAGE_ID_LEN];
    char title[APP_MAX_NAME_LEN];
    lv_obj_t *container;
    /* App pages can opt out of the bottom bar and get a top bar entry point
     * instead (see ui_pages_add_hidden). */
    bool in_nav;
} ui_page_entry_t;

static ui_page_entry_t s_pages[APP_MAX_PAGES];
static uint16_t s_page_count = 0;
static int16_t s_current_index = -1;
static ui_pages_show_cb_t s_show_cb = NULL;
static ui_pages_action_cb_t s_gear_cb = NULL;
static ui_pages_action_cb_t s_screen_built_cb = NULL;

void ui_pages_set_show_callback(ui_pages_show_cb_t cb)
{
    s_show_cb = cb;
}

void ui_pages_set_gear_callback(ui_pages_action_cb_t cb)
{
    s_gear_cb = cb;
}

void ui_pages_set_screen_built_callback(ui_pages_action_cb_t cb)
{
    s_screen_built_cb = cb;
}

static lv_obj_t *s_background = NULL;
static lv_obj_t *s_topbar = NULL;
static lv_obj_t *s_content_box = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_wifi_icon = NULL;
static lv_obj_t *s_api_icon = NULL;
static lv_obj_t *s_gear_icon = NULL;
/* Cameras shortcut; only shown while a cameras page is registered (see
 * ui_pages_set_camera_shortcut()). */
static char s_camera_page_id[APP_MAX_PAGE_ID_LEN] = {0};
static lv_obj_t *s_camera_icon = NULL;
/* Weather shortcut; only shown while a "pogoda" page is registered. */
static lv_obj_t *s_weather_icon = NULL;
/* HA Assist push-to-talk; only shown while Assist is enabled. */
static lv_obj_t *s_assist_icon = NULL;
static lv_obj_t *s_nav_bar = NULL;
static lv_obj_t *s_nav_home_button = NULL;
static lv_obj_t *s_nav_home_label = NULL;
static lv_obj_t *s_nav_extra_buttons[APP_MAX_PAGES - 1] = {0};
static lv_obj_t *s_nav_extra_labels[APP_MAX_PAGES - 1] = {0};
static uint16_t s_nav_extra_page_index[APP_MAX_PAGES - 1] = {0};

/* ---- Easter egg: 7 taps on the home nav button reveal a swimming Betta. */
#if LV_USE_LOTTIE && APP_UI_BETTA_LOTTIE_ASSET
extern const uint8_t betta_lottie_start[] asm("_binary_betta_json_start");
extern const uint8_t betta_lottie_end[]   asm("_binary_betta_json_end");
#define UI_BETTA_TAP_TARGET    7U
#define UI_BETTA_TAP_WINDOW_MS 2500U

static lv_obj_t *s_betta_overlay = NULL;
static lv_obj_t *s_betta_lottie  = NULL;
static void     *s_betta_buf     = NULL;
static uint8_t   s_betta_taps    = 0;
static uint32_t  s_betta_last_ms = 0;
#endif

#define TOPBAR_TIME_FONT APP_FONT_TEXT_34

#define TOPBAR_DATE_FONT APP_FONT_TEXT_22

#if LV_FONT_MONTSERRAT_24
#define TOPBAR_ICON_FONT (&lv_font_montserrat_24)
#elif LV_FONT_MONTSERRAT_20
#define TOPBAR_ICON_FONT (&lv_font_montserrat_20)
#else
#define TOPBAR_ICON_FONT LV_FONT_DEFAULT
#endif

/* ---- Configurable top bar (see the APP_DISPLAY_TOPBAR_* settings). ----
 * The elements are chained from both edges: the date and the gear from the
 * left, the status chips from the right, and the clock is centred in whatever
 * space is left.  The clock font shrinks (and finally the text is elided) so a
 * long time string can never run into the gear or the chips. */
#define TOPBAR_MARGIN 12
#define TOPBAR_GAP 10
/* Every icon chip of the top bar (gear, cameras, weather, assist, alert, HA,
 * network) is the same square; waste chips share the height. */
#define TOPBAR_CHIP_SIZE 34
#define TOPBAR_CLOCK_FONTS 4
/* Short budget for the once-a-minute clock and rare status updates: both run
 * from a context that already owns the display lock (see ui_runtime). */
#define TOPBAR_LOCK_TIMEOUT_MS 100U
/* The render task holds the display lock for the largest part of a frame, so a
 * user triggered change needs the same budget as the screenshot endpoint.
 * With a short timeout the update was dropped without any trace. */
#define TOPBAR_APPLY_LOCK_TIMEOUT_MS 1500U

typedef struct {
    bool loaded;
    bool show_clock;
    bool show_date;
    bool show_gear;
    bool show_status;
    bool icon_text;
    bool custom_colors;
    bool clock_24h;
    uint32_t bg_color;
    uint32_t clock_color;
    uint32_t date_color;
    uint32_t gear_color;
    uint32_t ha_color;
    uint32_t wifi_color;
    char date_format[24];
} ui_topbar_cfg_t;

static ui_topbar_cfg_t s_topbar_cfg = {
    .show_clock = APP_DISPLAY_TOPBAR_SHOW_CLOCK,
    .show_date = APP_DISPLAY_TOPBAR_SHOW_DATE,
    .show_gear = APP_DISPLAY_TOPBAR_SHOW_GEAR,
    .show_status = APP_DISPLAY_TOPBAR_SHOW_STATUS,
    .icon_text = APP_DISPLAY_TOPBAR_ICON_TEXT,
    .custom_colors = APP_DISPLAY_TOPBAR_CUSTOM_COLORS,
    .clock_24h = APP_DISPLAY_CLOCK_24H,
    .bg_color = APP_DISPLAY_TOPBAR_BG_COLOR,
    .clock_color = APP_DISPLAY_TOPBAR_CLOCK_COLOR,
    .date_color = APP_DISPLAY_TOPBAR_DATE_COLOR,
    .gear_color = APP_DISPLAY_TOPBAR_GEAR_COLOR,
    .ha_color = APP_DISPLAY_TOPBAR_HA_COLOR,
    .wifi_color = APP_DISPLAY_TOPBAR_WIFI_COLOR,
};

/* Bottom bar (page tabs) own-colour set. Filled from the same runtime settings
 * snapshot as the top bar so both are applied from one place. */
typedef struct {
    bool custom_colors;
    uint32_t bar_bg_color;
    uint32_t bar_border_color;
    uint32_t button_bg_color;
    uint32_t button_border_color;
    uint32_t tab_idle_color;
    uint32_t tab_active_color;
    uint32_t home_idle_color;
    uint32_t home_active_color;
} ui_nav_cfg_t;

static ui_nav_cfg_t s_nav_cfg = {
    .custom_colors = APP_DISPLAY_NAV_CUSTOM_COLORS,
    .bar_bg_color = APP_DISPLAY_NAV_BAR_BG_COLOR,
    .bar_border_color = APP_DISPLAY_NAV_BAR_BORDER_COLOR,
    .button_bg_color = APP_DISPLAY_NAV_BUTTON_BG_COLOR,
    .button_border_color = APP_DISPLAY_NAV_BUTTON_BORDER_COLOR,
    .tab_idle_color = APP_DISPLAY_NAV_TAB_IDLE_COLOR,
    .tab_active_color = APP_DISPLAY_NAV_TAB_ACTIVE_COLOR,
    .home_idle_color = APP_DISPLAY_NAV_HOME_IDLE_COLOR,
    .home_active_color = APP_DISPLAY_NAV_HOME_ACTIVE_COLOR,
};

static lv_color_t ui_nav_bar_bg_color(void)
{
    return lv_color_hex(s_nav_cfg.custom_colors ? s_nav_cfg.bar_bg_color : APP_UI_COLOR_TOPBAR_BG);
}

static lv_color_t ui_nav_bar_border_color(void)
{
    return lv_color_hex(s_nav_cfg.custom_colors ? s_nav_cfg.bar_border_color : APP_UI_COLOR_TOPBAR_BORDER);
}

static lv_color_t ui_nav_button_bg_color(void)
{
    return lv_color_hex(s_nav_cfg.custom_colors ? s_nav_cfg.button_bg_color : APP_UI_COLOR_TOPBAR_CHIP_BG);
}

static lv_color_t ui_nav_button_border_color(void)
{
    return lv_color_hex(s_nav_cfg.custom_colors ? s_nav_cfg.button_border_color : APP_UI_COLOR_TOPBAR_CHIP_BORDER);
}

static lv_color_t ui_nav_text_color(bool selected, bool is_home)
{
    if (is_home) {
        return lv_color_hex(s_nav_cfg.custom_colors
                                ? (selected ? s_nav_cfg.home_active_color : s_nav_cfg.home_idle_color)
                                : (selected ? APP_UI_COLOR_NAV_HOME_ACTIVE : APP_UI_COLOR_NAV_HOME_IDLE));
    }
    return lv_color_hex(s_nav_cfg.custom_colors
                            ? (selected ? s_nav_cfg.tab_active_color : s_nav_cfg.tab_idle_color)
                            : (selected ? APP_UI_COLOR_NAV_TAB_ACTIVE : APP_UI_COLOR_NAV_TAB_IDLE));
}
/* Last status received from the network layer, so the icons can be rebuilt when
 * a top bar setting changes. */
static bool s_status_wifi;
static bool s_status_wifi_ap;
static bool s_status_ha;
static bool s_status_ha_sync;
/* Last clock/date drawn, so a new second does not trigger another layout pass. */
static bool s_topbar_have_time;
static struct tm s_topbar_time;
static char s_topbar_time_text[32];
static char s_topbar_date_text[40];

/* ---- MDI topbar icon font (nice wifi / home-assistant / cog glyphs). ---- */
#ifndef APP_HAVE_MDI_TOPBAR_FONT
#define APP_HAVE_MDI_TOPBAR_FONT 0
#endif

#if APP_HAVE_MDI_TOPBAR_FONT
LV_FONT_DECLARE(mdi_topbar_24);
#define TOPBAR_MDI_FONT (&mdi_topbar_24)
#endif

/* Material Design Icons codepoints (materialdesignicons-webfont.ttf). */
#define MDI_CP_ACCESS_POINT   0x0F0003UL
#define MDI_CP_WIFI           0x0F05A9UL
#define MDI_CP_WIFI_OFF       0x0F05AAUL
#define MDI_CP_HOME_ASSISTANT 0x0F07D0UL
#define MDI_CP_COG            0x0F0493UL
#define MDI_CP_HOME           0x0F02DCUL
/* weather-lightning: cloud with a bolt - the "Pogoda" page shortcut. */
#define MDI_CP_WEATHER        0x0F0593UL

static void ui_pages_utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = '\0';
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = '\0';
    }
}

/* Set a topbar icon label: MDI glyph when available and enabled, otherwise the
 * fallback symbol/text. */
static void ui_pages_set_topbar_icon(lv_obj_t *label, uint32_t mdi_cp, const char *fallback)
{
    if (label == NULL) {
        return;
    }
#if APP_HAVE_MDI_TOPBAR_FONT
    if (!s_topbar_cfg.icon_text) {
        char buf[8] = {0};
        ui_pages_utf8_encode(mdi_cp, buf);
        lv_label_set_text(label, buf);
        lv_obj_set_style_text_font(label, TOPBAR_MDI_FONT, LV_PART_MAIN);
        return;
    }
#endif
    (void)mdi_cp;
    lv_label_set_text(label, fallback);
    lv_obj_set_style_text_font(label, TOPBAR_ICON_FONT, LV_PART_MAIN);
}

/* The MDI topbar subset has no camera glyph, so the cameras chip uses the
 * video symbol of LVGL's built-in Montserrat font instead. */
static void ui_pages_set_topbar_camera_icon(lv_obj_t *label)
{
    if (label == NULL) {
        return;
    }
#if APP_HAVE_MDI_TOPBAR_FONT
    if (!s_topbar_cfg.icon_text) {
        if (!ui_mdi_icon_apply(label, "cctv", 24)) {
            lv_label_set_text(label, LV_SYMBOL_VIDEO);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_20, LV_PART_MAIN);
        }
        return;
    }
#endif
    lv_label_set_text(label, ui_i18n_get("topbar.cameras", "CAM"));
    lv_obj_set_style_text_font(label, TOPBAR_ICON_FONT, LV_PART_MAIN);
}

#define NAV_TEXT_FONT APP_FONT_TEXT_16

static void ui_pages_style_nav_button(lv_obj_t *btn, lv_obj_t *label, bool selected, bool is_home)
{
    if (btn == NULL || label == NULL) {
        return;
    }

    const lv_color_t chip_bg = ui_nav_button_bg_color();
    const lv_color_t chip_border = ui_nav_button_border_color();
    lv_obj_set_style_bg_color(btn, chip_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, selected ? LV_OPA_80 : LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, selected ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, chip_border, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(btn, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (is_home) {
        lv_obj_set_style_text_color(label, ui_nav_text_color(selected, true), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_decor(label, LV_TEXT_DECOR_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_text_color(label, ui_nav_text_color(selected, false), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_decor(label, LV_TEXT_DECOR_NONE, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static int16_t ui_pages_find(const char *page_id)
{
    if (page_id == NULL) {
        return -1;
    }
    for (uint16_t i = 0; i < s_page_count; i++) {
        if (strncmp(page_id, s_pages[i].id, APP_MAX_PAGE_ID_LEN) == 0) {
            return (int16_t)i;
        }
    }
    return -1;
}

static void ui_topbar_update_camera_tint(uint16_t selected_index);

/* Left navigation: home on top, then one full-width row per room. */
static void ui_pages_layout_nav_left(const uint16_t *nav_page, uint16_t nav_count, uint16_t selected_index)
{
    const lv_coord_t margin = 10;
    const lv_coord_t gap = 8;
    const lv_coord_t btn_w = APP_NAV_LEFT_WIDTH - 2 * margin - 1;
    const lv_coord_t avail_h = APP_SCREEN_HEIGHT - APP_CONTENT_BOX_Y - 2 * margin;
    const uint16_t rows = (uint16_t)(nav_count + 1U);
    /* Rows share the height, but stay finger sized and never get huge. */
    lv_coord_t btn_h = (avail_h - (lv_coord_t)(rows - 1U) * gap) / (lv_coord_t)rows;
    const lv_coord_t max_h = (APP_SCREEN_HEIGHT >= 700) ? 72 : 56;
    if (btn_h > max_h) {
        btn_h = max_h;
    }
    if (btn_h < 44) {
        btn_h = 44;
    }

    lv_coord_t y = margin;
    lv_obj_set_size(s_nav_home_button, btn_w, btn_h);
    lv_obj_set_pos(s_nav_home_button, margin, y);
    ui_pages_style_nav_button(s_nav_home_button, s_nav_home_label, (selected_index == 0), true);
    lv_obj_clear_flag(s_nav_home_button, LV_OBJ_FLAG_HIDDEN);
    y += btn_h + gap;

    uint16_t slot = 0;
    for (uint16_t n = 0; n < nav_count && slot < (APP_MAX_PAGES - 1); n++, slot++) {
        const uint16_t page_index = nav_page[n];
        lv_obj_t *btn = s_nav_extra_buttons[slot];
        lv_obj_t *label = s_nav_extra_labels[slot];
        if (btn == NULL || label == NULL) {
            continue;
        }
        s_nav_extra_page_index[slot] = page_index;
        lv_label_set_text(label, s_pages[page_index].title);
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_set_pos(btn, margin, y);
        lv_obj_set_width(label, btn_w - 16);
        lv_obj_center(label);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
        ui_pages_style_nav_button(btn, label, (page_index == selected_index), false);
        y += btn_h + gap;
    }
    for (; slot < (APP_MAX_PAGES - 1); slot++) {
        s_nav_extra_page_index[slot] = APP_MAX_PAGES;
        if (s_nav_extra_buttons[slot] != NULL) {
            lv_obj_add_flag(s_nav_extra_buttons[slot], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_pages_apply_tab_style(uint16_t selected_index)
{
    ui_topbar_update_camera_tint(selected_index);

    if (s_nav_bar == NULL || s_nav_home_button == NULL || s_nav_home_label == NULL) {
        return;
    }

    const lv_coord_t nav_btn_h = 42;
    const lv_coord_t nav_home_w = 72;
    const lv_coord_t nav_btn_y = 9;
    const lv_coord_t nav_outer_margin = 14;
    const lv_coord_t nav_home_gap = 12;
    const lv_coord_t nav_side_gap = 8;
    const lv_coord_t nav_min_side_btn_w = 64;
    /* Home sits at the far left; the other tabs follow left to right in
     * layout order. */
    const lv_coord_t nav_home_x = nav_outer_margin;

    lv_obj_set_size(s_nav_home_button, nav_home_w, nav_btn_h);
    lv_obj_set_pos(s_nav_home_button, nav_home_x, nav_btn_y);
    ui_pages_style_nav_button(s_nav_home_button, s_nav_home_label, (selected_index == 0), true);
    lv_obj_clear_flag(s_nav_home_button, LV_OBJ_FLAG_HIDDEN);

    /* Only pages that opted into the bottom bar get a tab; app pages such as
     * the radio are reached from the top bar instead. */
    uint16_t nav_page[APP_MAX_PAGES] = {0};
    uint16_t nav_count = 0;
    for (uint16_t page_index = 1; page_index < s_page_count; page_index++) {
        if (s_pages[page_index].in_nav) {
            nav_page[nav_count++] = page_index;
        }
    }

    if (ui_geometry_nav_left()) {
        ui_pages_layout_nav_left(nav_page, nav_count, selected_index);
        return;
    }

    lv_coord_t tabs_start = nav_home_x + nav_home_w + nav_home_gap;
    lv_coord_t tabs_end = APP_SCREEN_WIDTH - nav_outer_margin;
    lv_coord_t tabs_region_w = (tabs_end > tabs_start) ? (tabs_end - tabs_start) : 0;

    lv_coord_t tab_btn_w = 0;
    if (nav_count > 0) {
        tab_btn_w = (tabs_region_w - ((lv_coord_t)nav_count - 1) * nav_side_gap) / (lv_coord_t)nav_count;
        if (tab_btn_w < nav_min_side_btn_w) {
            tab_btn_w = nav_min_side_btn_w;
        }
    }

    uint16_t slot = 0;
    for (uint16_t n = 0; n < nav_count && slot < (APP_MAX_PAGES - 1); n++, slot++) {
        uint16_t page_index = nav_page[n];
        lv_obj_t *btn = s_nav_extra_buttons[slot];
        lv_obj_t *label = s_nav_extra_labels[slot];
        if (btn == NULL || label == NULL) {
            continue;
        }

        lv_coord_t w = tab_btn_w;
        lv_coord_t x = tabs_start + (lv_coord_t)n * (w + nav_side_gap);

        s_nav_extra_page_index[slot] = page_index;
        lv_label_set_text(label, s_pages[page_index].title);
        lv_obj_set_size(btn, w, nav_btn_h);
        lv_obj_set_pos(btn, x, nav_btn_y);
        lv_obj_set_width(label, w - 20);
        lv_obj_center(label);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
        ui_pages_style_nav_button(btn, label, (page_index == selected_index), false);
    }

    for (; slot < (APP_MAX_PAGES - 1); slot++) {
        s_nav_extra_page_index[slot] = APP_MAX_PAGES;
        if (s_nav_extra_buttons[slot] != NULL) {
            lv_obj_add_flag(s_nav_extra_buttons[slot], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_pages_style_topbar_chip(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(obj, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_left(obj, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(obj, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(obj, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(obj, 4, LV_PART_MAIN);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(obj, TOPBAR_ICON_FONT, LV_PART_MAIN);
}

/* Square icon chip: chip look, fixed TOPBAR_CHIP_SIZE, glyph centred whatever
 * font it uses (MDI subset, tiny_ttf MDI, Montserrat symbol). */
static void ui_topbar_square_chip(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(obj, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 10, LV_PART_MAIN);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    lv_coord_t pad = (font != NULL) ? (TOPBAR_CHIP_SIZE - 2 - lv_font_get_line_height(font)) / 2 : 0;
    if (pad < 0) {
        pad = 0;
    }
    lv_obj_set_style_pad_left(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(obj, pad, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(obj, 0, LV_PART_MAIN);
    lv_obj_set_size(obj, TOPBAR_CHIP_SIZE, TOPBAR_CHIP_SIZE);
}

static void ui_topbar_set_visible(lv_obj_t *obj, bool visible)
{
    if (obj == NULL) {
        return;
    }
    /* LVGL's add/clear already early-out, but only after walking the flag word;
     * the callers here run on every render, so skip the work outright. */
    const bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (hidden == !visible) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool ui_topbar_logos_active(void)
{
#if APP_HAVE_MDI_TOPBAR_FONT
    return !s_topbar_cfg.icon_text;
#else
    return false;
#endif
}

/* Theme owns the top bar unless the user switched on the own colour set. */
static lv_color_t ui_topbar_text_color(uint32_t custom, uint32_t from_theme)
{
    return lv_color_hex(s_topbar_cfg.custom_colors ? custom : from_theme);
}

static lv_color_t ui_topbar_status_color(bool on, uint32_t custom_on)
{
    if (on && s_topbar_cfg.custom_colors) {
        return lv_color_hex(custom_on);
    }
    return lv_color_hex(on ? (uint32_t)APP_UI_COLOR_TOPBAR_STATUS_ON : (uint32_t)APP_UI_COLOR_TOPBAR_STATUS_OFF);
}

static bool ui_topbar_camera_available(void)
{
    return s_camera_icon != NULL && s_camera_page_id[0] != '\0' && ui_pages_find(s_camera_page_id) >= 0;
}

/* The cameras shortcut mirrors the bottom bar tabs: muted while another page
 * is active, highlighted while the cameras page is on screen. */
static void ui_topbar_update_camera_tint(uint16_t selected_index)
{
    if (s_camera_icon == NULL) {
        return;
    }
    const int16_t camera_index = (s_camera_page_id[0] != '\0') ? ui_pages_find(s_camera_page_id) : -1;
    const bool active = (camera_index >= 0) && ((uint16_t)camera_index == selected_index);
    lv_obj_set_style_text_color(
        s_camera_icon,
        active ? lv_color_hex(APP_UI_COLOR_TOPBAR_TEXT)
               : ui_topbar_text_color(s_topbar_cfg.gear_color, (uint32_t)APP_UI_COLOR_TOPBAR_MUTED),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_camera_icon, active ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_camera_icon, active ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static bool ui_topbar_weather_available(void)
{
    return s_weather_icon != NULL && ui_pages_find(UI_WEATHER_PAGE_ID) >= 0;
}

/* Same highlight rule for the weather shortcut. */
static void ui_topbar_update_weather_tint(uint16_t selected_index)
{
    if (s_weather_icon == NULL) {
        return;
    }
    const int16_t weather_index = ui_pages_find(UI_WEATHER_PAGE_ID);
    const bool active = (weather_index >= 0) && ((uint16_t)weather_index == selected_index);
    lv_obj_set_style_text_color(
        s_weather_icon,
        active ? lv_color_hex(APP_UI_COLOR_TOPBAR_TEXT)
               : ui_topbar_text_color(s_topbar_cfg.gear_color, (uint32_t)APP_UI_COLOR_TOPBAR_MUTED),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_weather_icon, active ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_weather_icon, active ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void ui_topbar_clock_text(char *buf, size_t len)
{
    if (!s_topbar_have_time) {
        snprintf(buf, len, "--:--");
        return;
    }
    const struct tm *now = &s_topbar_time;
    if (s_topbar_cfg.clock_24h) {
        snprintf(buf, len, "%02d:%02d", now->tm_hour, now->tm_min);
        return;
    }
    int hour = now->tm_hour % 12;
    if (hour == 0) {
        hour = 12;
    }
    snprintf(buf, len, "%d:%02d %s", hour, now->tm_min, now->tm_hour < 12 ? "AM" : "PM");
}

static void ui_topbar_render_status(void)
{
    if (s_wifi_icon != NULL) {
        char wifi_text[32] = {0};
        bool wifi_on = false;
        uint32_t mdi_cp = MDI_CP_WIFI_OFF;
        if (s_status_wifi_ap) {
            snprintf(wifi_text, sizeof(wifi_text), "%s", ui_i18n_get("topbar.ap", "AP"));
            mdi_cp = MDI_CP_ACCESS_POINT;
            wifi_on = true;
        } else if (s_status_wifi) {
            snprintf(wifi_text, sizeof(wifi_text), "%s", ui_i18n_get("topbar.wifi", "Wi-Fi"));
            mdi_cp = MDI_CP_WIFI;
            wifi_on = true;
        } else {
            snprintf(wifi_text, sizeof(wifi_text), "%s", ui_i18n_get("topbar.wifi_off", "Wi-Fi off"));
        }
#if CONFIG_APP_PANEL_BOARD_WAVESHARE_P4ETH101
        /* No Wi-Fi on this board: the chip shows the wired link. */
        (void)mdi_cp;
        if (s_topbar_cfg.icon_text ||
            !ui_mdi_icon_apply(s_wifi_icon, s_status_wifi ? "ethernet" : "ethernet-off", 24)) {
            lv_label_set_text(s_wifi_icon, s_status_wifi ? "LAN" : ui_i18n_get("topbar.lan_off", "LAN aus"));
            lv_obj_set_style_text_font(s_wifi_icon, TOPBAR_ICON_FONT, LV_PART_MAIN);
        }
#else
        ui_pages_set_topbar_icon(s_wifi_icon, mdi_cp, wifi_text);
#endif
        lv_obj_set_style_text_color(s_wifi_icon, ui_topbar_status_color(wifi_on, s_topbar_cfg.wifi_color), LV_PART_MAIN);
    }

    if (s_api_icon != NULL) {
        char api_text[32] = {0};
        const char *label = ui_i18n_get("topbar.ha", "HA");
        bool api_on = s_status_ha;
        if (s_status_ha) {
            if (s_status_ha_sync) {
                snprintf(api_text, sizeof(api_text), "%s", label);
            } else {
                snprintf(api_text, sizeof(api_text), "%s", ui_i18n_get("topbar.ha_sync", "HA sync"));
            }
        } else {
            snprintf(api_text, sizeof(api_text), "%s", ui_i18n_get("topbar.ha_off", "HA off"));
        }
        ui_pages_set_topbar_icon(s_api_icon, MDI_CP_HOME_ASSISTANT, api_text);
        lv_obj_set_style_text_color(s_api_icon, ui_topbar_status_color(api_on, s_topbar_cfg.ha_color), LV_PART_MAIN);
    }

    /* The gear label is created with a glyph, so it has to follow the icon mode
     * as well - otherwise it keeps the logo while the status chips switch. */
    if (s_gear_icon != NULL) {
        ui_pages_set_topbar_icon(s_gear_icon, MDI_CP_COG, ui_i18n_get("topbar.settings", "SET"));
    }

    if (s_camera_icon != NULL) {
        ui_pages_set_topbar_camera_icon(s_camera_icon);
    }

    if (s_weather_icon != NULL) {
        ui_pages_set_topbar_icon(s_weather_icon, MDI_CP_WEATHER, ui_i18n_get("topbar.weather", "POGODA"));
    }
}

static void ui_topbar_apply_style(void)
{
    if (s_topbar == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(
        s_topbar, ui_topbar_text_color(s_topbar_cfg.bg_color, (uint32_t)APP_UI_COLOR_TOPBAR_BG), LV_PART_MAIN);
    if (s_date_label != NULL) {
        lv_obj_set_style_text_color(
            s_date_label, ui_topbar_text_color(s_topbar_cfg.date_color, (uint32_t)APP_UI_COLOR_TOPBAR_MUTED), LV_PART_MAIN);
    }
    if (s_gear_icon != NULL) {
        lv_obj_set_style_text_color(
            s_gear_icon, ui_topbar_text_color(s_topbar_cfg.gear_color, (uint32_t)APP_UI_COLOR_TOPBAR_MUTED), LV_PART_MAIN);
    }
    ui_topbar_update_camera_tint((uint16_t)(s_current_index > 0 ? s_current_index : 0));
    ui_topbar_update_weather_tint((uint16_t)(s_current_index > 0 ? s_current_index : 0));
    if (s_time_label != NULL) {
        lv_obj_set_style_text_color(
            s_time_label, ui_topbar_text_color(s_topbar_cfg.clock_color, (uint32_t)APP_UI_COLOR_TOPBAR_TEXT), LV_PART_MAIN);
    }
}

/* Chain the elements from the left (date, cameras, weather) and from the right
 * (status chips, settings), then fit the clock into the gap that is left
 * over. */
/* ---- Top bar arrangement ------------------------------------------------- */

typedef enum {
    TB_DATE = 0,
    TB_CLOCK,
    TB_CAMERAS,
    TB_WEATHER,
    TB_WASTE,
    TB_ALERT,
    TB_ASSIST,
    TB_GEAR,
    TB_HA,
    TB_NETWORK,
    TB_COUNT
} tb_item_t;

enum { TB_ZONE_LEFT = 0, TB_ZONE_CENTER = 1, TB_ZONE_RIGHT = 2, TB_ZONE_HIDDEN = 3 };

static const char *const k_tb_names[TB_COUNT] = {
    "date", "clock", "cameras", "weather", "waste", "alert", "assist", "gear", "ha", "network"};
static const uint8_t k_tb_default_zone[TB_COUNT] = {
    TB_ZONE_LEFT, TB_ZONE_CENTER, TB_ZONE_LEFT, TB_ZONE_LEFT, TB_ZONE_LEFT,
    TB_ZONE_RIGHT, TB_ZONE_RIGHT, TB_ZONE_RIGHT, TB_ZONE_RIGHT, TB_ZONE_RIGHT};

/* Display order (left to right within each zone) and zone of every element. */
static tb_item_t s_tb_order[TB_COUNT] = {
    TB_DATE, TB_CAMERAS, TB_WEATHER, TB_WASTE, TB_CLOCK, TB_ALERT, TB_ASSIST, TB_GEAR, TB_HA, TB_NETWORK};
static uint8_t s_tb_zone[TB_COUNT] = {
    TB_ZONE_LEFT, TB_ZONE_CENTER, TB_ZONE_LEFT, TB_ZONE_LEFT, TB_ZONE_LEFT,
    TB_ZONE_RIGHT, TB_ZONE_RIGHT, TB_ZONE_RIGHT, TB_ZONE_RIGHT, TB_ZONE_RIGHT};

/* "L:date,cameras;C:clock;R:alert,gear;H:weather" - elements missing from the
 * spec keep their default zone and go after the listed ones. */
static void ui_topbar_parse_layout(const char *spec)
{
    static const tb_item_t k_default_order[TB_COUNT] = {
        TB_DATE, TB_CAMERAS, TB_WEATHER, TB_WASTE, TB_CLOCK, TB_ALERT, TB_ASSIST, TB_GEAR, TB_HA, TB_NETWORK};
    bool placed[TB_COUNT] = {false};
    int n = 0;
    if (spec != NULL) {
        char buf[RUNTIME_TOPBAR_LAYOUT_LEN];
        snprintf(buf, sizeof(buf), "%s", spec);
        char *save_zone = NULL;
        for (char *part = strtok_r(buf, ";", &save_zone); part != NULL; part = strtok_r(NULL, ";", &save_zone)) {
            if (part[0] == '\0' || part[1] != ':') {
                continue;
            }
            const char zc = part[0];
            const int zone = zc == 'L' ? TB_ZONE_LEFT : zc == 'C' ? TB_ZONE_CENTER : zc == 'R' ? TB_ZONE_RIGHT
                           : zc == 'H' ? TB_ZONE_HIDDEN : -1;
            if (zone < 0) {
                continue;
            }
            char *save_item = NULL;
            for (char *name = strtok_r(part + 2, ",", &save_item); name != NULL;
                 name = strtok_r(NULL, ",", &save_item)) {
                for (int i = 0; i < TB_COUNT; i++) {
                    if (!placed[i] && strcmp(name, k_tb_names[i]) == 0) {
                        placed[i] = true;
                        s_tb_zone[i] = (uint8_t)zone;
                        s_tb_order[n++] = (tb_item_t)i;
                    }
                }
            }
        }
    }
    for (int k = 0; k < TB_COUNT; k++) {
        const tb_item_t it = k_default_order[k];
        if (!placed[it]) {
            s_tb_zone[it] = k_tb_default_zone[it];
            s_tb_order[n++] = it;
        }
    }
}

static lv_obj_t *ui_topbar_item_obj(tb_item_t it)
{
    switch (it) {
    case TB_DATE: return s_date_label;
    case TB_CLOCK: return s_time_label;
    case TB_CAMERAS: return s_camera_icon;
    case TB_WEATHER: return s_weather_icon;
    case TB_ALERT: return ui_topbar_alerts_chip();
    case TB_ASSIST: return s_assist_icon;
    case TB_GEAR: return s_gear_icon;
    case TB_HA: return s_api_icon;
    case TB_NETWORK: return s_wifi_icon;
    default: return NULL;
    }
}

static lv_coord_t ui_topbar_item_width(tb_item_t it)
{
    if (it == TB_WASTE) {
        return ui_topbar_waste_width(TOPBAR_GAP);
    }
    lv_obj_t *obj = ui_topbar_item_obj(it);
    return obj != NULL ? lv_obj_get_width(obj) : 0;
}

static void ui_topbar_apply_layout(void)
{
    if (s_topbar == NULL) {
        return;
    }

    const bool show_date = s_topbar_cfg.show_date && s_date_label != NULL;
    const bool show_gear = s_topbar_cfg.show_gear && s_gear_icon != NULL;
    /* The cameras and weather shortcuts are app entry points, not part of the
     * configurable status group: each appears as soon as its page exists. */
    const bool show_camera = ui_topbar_camera_available();
    const bool show_weather = ui_topbar_weather_available();
    const bool show_assist = s_assist_icon != NULL && panel_assist_is_enabled();
    const bool show_clock = s_topbar_cfg.show_clock && s_time_label != NULL;
    const bool show_status = s_topbar_cfg.show_status;

    ui_topbar_set_visible(s_date_label, show_date);
    ui_topbar_set_visible(s_gear_icon, show_gear);
    ui_topbar_set_visible(s_camera_icon, show_camera);
    ui_topbar_set_visible(s_weather_icon, show_weather);
    ui_topbar_set_visible(s_assist_icon, show_assist);
    ui_topbar_set_visible(s_time_label, show_clock);
    ui_topbar_set_visible(s_api_icon, show_status && s_api_icon != NULL);
    ui_topbar_set_visible(s_wifi_icon, show_status && s_wifi_icon != NULL);

    /* Chips without a fixed width, so a glyph chip does not steal room from the
     * clock and a word chip gets exactly the space it needs. */
    if (s_date_label != NULL) {
        lv_obj_set_width(s_date_label, LV_SIZE_CONTENT);
    }
    lv_obj_t *const glyph_chips[] = {
        s_gear_icon, s_camera_icon, s_weather_icon, s_api_icon, s_wifi_icon, s_assist_icon, ui_topbar_alerts_chip()};
    for (size_t i = 0; i < sizeof(glyph_chips) / sizeof(glyph_chips[0]); i++) {
        lv_obj_t *chip = glyph_chips[i];
        if (chip == NULL) {
            continue;
        }
        const bool word_chip = s_topbar_cfg.icon_text &&
                               (chip == s_gear_icon || chip == s_camera_icon || chip == s_weather_icon ||
                                   chip == s_api_icon || chip == s_wifi_icon);
        if (word_chip) {
            ui_pages_style_topbar_chip(chip);
            lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        } else {
            ui_topbar_square_chip(chip);
        }
    }
    lv_obj_update_layout(s_topbar);

    /* ---- arrangement: left group, centre group (around the screen centre),
     * right group; the clock takes the largest font that fits. ---- */
    ui_topbar_waste_set_suppressed(s_tb_zone[TB_WASTE] == TB_ZONE_HIDDEN);
    ui_topbar_alerts_set_suppressed(s_tb_zone[TB_ALERT] == TB_ZONE_HIDDEN);
    bool visible[TB_COUNT] = {
        [TB_DATE] = show_date,
        [TB_CLOCK] = show_clock,
        [TB_CAMERAS] = show_camera,
        [TB_WEATHER] = show_weather,
        [TB_WASTE] = ui_topbar_waste_width(TOPBAR_GAP) > 0,
        [TB_ALERT] = ui_topbar_alerts_visible(),
        [TB_ASSIST] = show_assist,
        [TB_GEAR] = show_gear,
        [TB_HA] = show_status && s_api_icon != NULL,
        [TB_NETWORK] = show_status && s_wifi_icon != NULL,
    };
    for (int i = 0; i < TB_COUNT; i++) {
        if (s_tb_zone[i] == TB_ZONE_HIDDEN) {
            visible[i] = false;
            if (i != TB_WASTE && i != TB_ALERT) {
                ui_topbar_set_visible(ui_topbar_item_obj((tb_item_t)i), false);
            }
        }
    }

    /* Squeeze levels: first the clock shrinks, then the date. */
    static const struct {
        uint8_t clock;
        uint8_t date;
    } k_levels[] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {3, 1}, {3, 2}, {3, 3}};
    const size_t level_count = sizeof(k_levels) / sizeof(k_levels[0]);
    const lv_font_t *clock_fonts[4] = {APP_FONT_TEXT_34, APP_FONT_TEXT_28, APP_FONT_TEXT_24, APP_FONT_TEXT_22};
    const lv_font_t *date_fonts[4] = {TOPBAR_DATE_FONT, APP_FONT_TEXT_20, APP_FONT_TEXT_18, APP_FONT_TEXT_16};
    lv_coord_t zone_w[3] = {0, 0, 0};

    /* Applies a squeeze level and measures the three groups. */
#define TB_MEASURE(level)                                                                                   \
    do {                                                                                                    \
        if (visible[TB_CLOCK]) {                                                                            \
            lv_obj_set_style_text_font(s_time_label, clock_fonts[k_levels[(level)].clock], LV_PART_MAIN);   \
            lv_obj_set_width(s_time_label, LV_SIZE_CONTENT);                                                \
            lv_label_set_long_mode(s_time_label, LV_LABEL_LONG_WRAP);                                       \
            lv_obj_update_layout(s_time_label);                                                             \
        }                                                                                                   \
        if (visible[TB_DATE]) {                                                                             \
            lv_obj_set_style_text_font(s_date_label, date_fonts[k_levels[(level)].date], LV_PART_MAIN);     \
            lv_obj_update_layout(s_date_label);                                                             \
        }                                                                                                   \
        for (int z_ = 0; z_ < 3; z_++) {                                                                    \
            zone_w[z_] = 0;                                                                                 \
            int n_ = 0;                                                                                     \
            for (int k_ = 0; k_ < TB_COUNT; k_++) {                                                         \
                const tb_item_t it_ = s_tb_order[k_];                                                       \
                if (!visible[it_] || s_tb_zone[it_] != z_) {                                                \
                    continue;                                                                               \
                }                                                                                           \
                zone_w[z_] += ui_topbar_item_width(it_) + (n_ > 0 ? TOPBAR_GAP : 0);                        \
                n_++;                                                                                       \
            }                                                                                               \
        }                                                                                                   \
    } while (0)

    /* Edges of the free space between the left and the right group. */
#define TB_LEFT_END() (TOPBAR_MARGIN + zone_w[TB_ZONE_LEFT] + (zone_w[TB_ZONE_LEFT] > 0 ? TOPBAR_GAP : 0))
#define TB_RIGHT_START() \
    (APP_SCREEN_WIDTH - TOPBAR_MARGIN - zone_w[TB_ZONE_RIGHT] - (zone_w[TB_ZONE_RIGHT] > 0 ? TOPBAR_GAP : 0))

    bool exact = true;  /* centre group exactly on the screen centre */
    bool found = false;
    size_t level = 0;
    /* 1) exact centre, shrinking the clock and then the date */
    for (level = 0; level < level_count && !found; level++) {
        TB_MEASURE(level);
        const lv_coord_t cw = zone_w[TB_ZONE_CENTER];
        if (cw == 0) {
            found = TB_LEFT_END() <= TB_RIGHT_START();
        } else {
            const lv_coord_t c_start = APP_SCREEN_WIDTH / 2 - cw / 2;
            found = TB_LEFT_END() <= c_start && c_start + cw <= TB_RIGHT_START();
        }
    }
    /* 2) centred in the free gap instead, with the largest fonts that fit */
    if (!found && zone_w[TB_ZONE_CENTER] > 0) {
        exact = false;
        for (level = 0; level < level_count && !found; level++) {
            TB_MEASURE(level);
            found = TB_LEFT_END() + zone_w[TB_ZONE_CENTER] <= TB_RIGHT_START();
        }
    }
    if (!found) {
        TB_MEASURE(level_count - 1);
    }

    lv_coord_t x[3] = {
        TOPBAR_MARGIN,
        APP_SCREEN_WIDTH / 2 - zone_w[TB_ZONE_CENTER] / 2,
        APP_SCREEN_WIDTH - TOPBAR_MARGIN - zone_w[TB_ZONE_RIGHT],
    };
    if (zone_w[TB_ZONE_CENTER] > 0) {
        const lv_coord_t left_end = TB_LEFT_END();
        const lv_coord_t right_start = TB_RIGHT_START();
        if (!exact) {
            x[TB_ZONE_CENTER] = left_end + (right_start - left_end - zone_w[TB_ZONE_CENTER]) / 2;
        }
        /* never overlap a side group */
        if (x[TB_ZONE_CENTER] < left_end) {
            x[TB_ZONE_CENTER] = left_end;
        }
        if (x[TB_ZONE_CENTER] + zone_w[TB_ZONE_CENTER] > right_start) {
            x[TB_ZONE_CENTER] = right_start - zone_w[TB_ZONE_CENTER];
        }
    }
#undef TB_MEASURE
#undef TB_LEFT_END
#undef TB_RIGHT_START
    for (int k = 0; k < TB_COUNT; k++) {
        const tb_item_t it = s_tb_order[k];
        const int z = s_tb_zone[it];
        if (!visible[it] || z == TB_ZONE_HIDDEN) {
            continue;
        }
        if (it == TB_WASTE) {
            x[z] = ui_topbar_waste_layout(x[z], TOPBAR_GAP);
            continue;
        }
        lv_obj_t *obj = ui_topbar_item_obj(it);
        lv_obj_align(obj, LV_ALIGN_LEFT_MID, x[z], 0);
        x[z] += lv_obj_get_width(obj) + TOPBAR_GAP;
    }
    lv_obj_update_layout(s_topbar);
}

void ui_pages_apply_topbar_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    s_topbar_cfg.loaded = true;
    ui_topbar_waste_apply_settings(settings);
    ui_topbar_alerts_apply_settings(settings);
    ui_topbar_parse_layout(settings->display_topbar_layout);
    const bool date_format_changed = strcmp(s_topbar_cfg.date_format, settings->display_topbar_date_format) != 0;
    snprintf(s_topbar_cfg.date_format, sizeof(s_topbar_cfg.date_format), "%s", settings->display_topbar_date_format);
    if (date_format_changed && s_topbar_have_time) {
        s_topbar_date_text[0] = '\0'; /* rewrite the label in the new format */
        ui_pages_set_topbar_datetime(&s_topbar_time);
    }
    s_topbar_cfg.show_clock = settings->display_topbar_show_clock;
    s_topbar_cfg.show_date = settings->display_topbar_show_date;
    s_topbar_cfg.show_gear = settings->display_topbar_show_gear;
    s_topbar_cfg.show_status = settings->display_topbar_show_status;
    s_topbar_cfg.icon_text = settings->display_topbar_icon_text;
    s_topbar_cfg.custom_colors = settings->display_topbar_custom_colors;
    s_topbar_cfg.clock_24h = settings->display_clock_24h;
    s_topbar_cfg.bg_color = settings->display_topbar_bg_color;
    s_topbar_cfg.clock_color = settings->display_topbar_clock_color;
    s_topbar_cfg.date_color = settings->display_topbar_date_color;
    s_topbar_cfg.gear_color = settings->display_topbar_gear_color;
    s_topbar_cfg.ha_color = settings->display_topbar_ha_color;
    s_topbar_cfg.wifi_color = settings->display_topbar_wifi_color;

    system_log_write_info(
        TAG_TOPBAR,
        "top bar clock %u date %u gear %u status %u, %s icons, %s colours",
        (unsigned)s_topbar_cfg.show_clock,
        (unsigned)s_topbar_cfg.show_date,
        (unsigned)s_topbar_cfg.show_gear,
        (unsigned)s_topbar_cfg.show_status,
        ui_topbar_logos_active() ? "logo" : "text",
        s_topbar_cfg.custom_colors ? "own" : "theme");

    if (!display_lock(TOPBAR_APPLY_LOCK_TIMEOUT_MS)) {
        system_log_write(TAG_TOPBAR, "top bar settings not applied: display lock busy");
        return;
    }
    ui_topbar_apply_style();
    ui_topbar_render_status();
    ui_topbar_apply_layout();
    display_unlock();
}

void ui_pages_refresh_topbar(void)
{
    if (s_topbar == NULL) {
        return;
    }
    if (!display_lock(TOPBAR_APPLY_LOCK_TIMEOUT_MS)) {
        system_log_write(TAG_TOPBAR, "top bar not re-laid out: display lock busy");
        return;
    }
    ui_topbar_apply_layout();
    display_unlock();
}

static void ui_nav_apply_style(void)
{
    if (s_nav_bar == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(s_nav_bar, ui_nav_bar_bg_color(), LV_PART_MAIN | LV_STATE_DEFAULT);
    /* The bar border colour drives the 1 px top separator (see ui_pages_create_nav). */
    lv_obj_set_style_border_color(s_nav_bar, ui_nav_bar_border_color(), LV_PART_MAIN | LV_STATE_DEFAULT);
    /* Re-lays out and re-colours every tab, including the home button. */
    ui_pages_apply_tab_style(s_current_index > 0 ? (uint16_t)s_current_index : 0);
}

void ui_pages_apply_bottom_bar_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    s_nav_cfg.custom_colors = settings->display_nav_custom_colors;
    s_nav_cfg.bar_bg_color = settings->display_nav_bar_bg_color;
    s_nav_cfg.bar_border_color = settings->display_nav_bar_border_color;
    s_nav_cfg.button_bg_color = settings->display_nav_button_bg_color;
    s_nav_cfg.button_border_color = settings->display_nav_button_border_color;
    s_nav_cfg.tab_idle_color = settings->display_nav_tab_idle_color;
    s_nav_cfg.tab_active_color = settings->display_nav_tab_active_color;
    s_nav_cfg.home_idle_color = settings->display_nav_home_idle_color;
    s_nav_cfg.home_active_color = settings->display_nav_home_active_color;

    system_log_write_info(TAG_NAVBAR, "bottom bar %s colours", s_nav_cfg.custom_colors ? "own" : "theme");

    if (s_nav_bar == NULL) {
        /* Bar not built yet; ui_pages_create_nav() reads the values cached above. */
        return;
    }
    if (!display_lock(TOPBAR_APPLY_LOCK_TIMEOUT_MS)) {
        system_log_write(TAG_NAVBAR, "bottom bar settings not applied: display lock busy");
        return;
    }
    ui_nav_apply_style();
    display_unlock();
}

static void ui_topbar_load_settings(void)
{
    /* The settings file read needs a whole runtime_settings_t plus the LittleFS/cJSON
     * scratch space. Keeping it off the stack matters: this runs from ui_pages_init()
     * during the page build, which is already deeply nested on the main task. Page
     * builds always hold the display lock, so the shared buffer cannot be used twice
     * at the same time. */
    static runtime_settings_t settings;
    runtime_settings_set_defaults(&settings);
    if (runtime_settings_load(&settings) != ESP_OK) {
        runtime_settings_set_defaults(&settings);
    }
    ui_pages_apply_topbar_settings(&settings);
    /* The bottom bar is created later in ui_pages_init(), so this only caches the
     * values; create_nav() picks them up when it builds the bar. */
    ui_pages_apply_bottom_bar_settings(&settings);
}

#if LV_USE_LOTTIE && APP_UI_BETTA_LOTTIE_ASSET
static void ui_pages_betta_hide(void);

static void ui_pages_betta_dismiss_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    ui_pages_betta_hide();
}

static void ui_pages_betta_show(void)
{
    if (s_betta_overlay != NULL) {
        return;
    }
    lv_obj_t *screen = lv_scr_act();
    if (screen == NULL) {
        return;
    }

    lv_coord_t side = APP_SCREEN_HEIGHT < APP_SCREEN_WIDTH ? APP_SCREEN_HEIGHT : APP_SCREEN_WIDTH;
    side -= 120;
    /* ThorVG renders the lottie on the CPU. Pixel cost scales quadratically;
     * the weather tiles run smoothly at ~130 px, so keep the betta close to that
     * even though the screen is much larger. */
    if (side > 220) {
        side = 220;
    }
    if (side < 160) {
        side = 160;
    }

    size_t buf_bytes = (size_t)side * (size_t)side * 4U + (size_t)LV_DRAW_BUF_ALIGN;
    s_betta_buf = lv_malloc(buf_bytes);
    if (s_betta_buf == NULL) {
        return;
    }
    memset(s_betta_buf, 0, buf_bytes);

    s_betta_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_betta_overlay);
    lv_obj_set_size(s_betta_overlay, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
    lv_obj_set_pos(s_betta_overlay, 0, 0);
    lv_obj_clear_flag(s_betta_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_betta_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_betta_overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_add_flag(s_betta_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_betta_overlay, ui_pages_betta_dismiss_cb, LV_EVENT_CLICKED, NULL);

    s_betta_lottie = lv_lottie_create(s_betta_overlay);
    lv_lottie_set_buffer(s_betta_lottie, side, side, s_betta_buf);
    lv_lottie_set_src_data(s_betta_lottie, betta_lottie_start,
                           (size_t)(betta_lottie_end - betta_lottie_start));
    lv_obj_set_size(s_betta_lottie, side, side);
    lv_obj_center(s_betta_lottie);

    lv_obj_move_foreground(s_betta_overlay);
}

static void ui_pages_betta_hide(void)
{
    if (s_betta_overlay != NULL) {
        lv_obj_del(s_betta_overlay);
        s_betta_overlay = NULL;
        s_betta_lottie  = NULL;
    }
    if (s_betta_buf != NULL) {
        lv_free(s_betta_buf);
        s_betta_buf = NULL;
    }
    s_betta_taps    = 0U;
    s_betta_last_ms = 0U;
}
#endif

static void ui_nav_home_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_page_count == 0) {
        return;
    }
    ui_pages_show_index(0);
#if LV_USE_LOTTIE && APP_UI_BETTA_LOTTIE_ASSET
    /* Easter egg: count taps on the home button. */
    uint32_t now = lv_tick_get();
    if (s_betta_last_ms != 0U && (now - s_betta_last_ms) > UI_BETTA_TAP_WINDOW_MS) {
        s_betta_taps = 0U;
    }
    s_betta_last_ms = now;
    s_betta_taps++;
    if (s_betta_taps >= UI_BETTA_TAP_TARGET) {
        s_betta_taps = 0U;
        ui_pages_betta_show();
    }
#endif
}

static void ui_nav_extra_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    uintptr_t slot = (uintptr_t)lv_event_get_user_data(event);
    if (slot >= (APP_MAX_PAGES - 1)) {
        return;
    }

    uint16_t page_index = s_nav_extra_page_index[slot];
    if (page_index >= s_page_count) {
        return;
    }
    ui_pages_show_index(page_index);
}

static void ui_topbar_gear_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    /* The gear opens the one settings overlay; a registered hook overrides it. */
    if (s_gear_cb != NULL) {
        s_gear_cb();
        return;
    }
    ui_settings_toggle();
}

static void ui_topbar_camera_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    system_log_event("ui", "topbar cameras tapped");
    if (s_camera_page_id[0] == '\0' || !ui_pages_show(s_camera_page_id)) {
        system_log_write_info(TAG_TOPBAR, "cameras page not registered");
    }
}

static void ui_topbar_weather_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    system_log_event("ui", "topbar weather tapped");
    if (!ui_pages_show(UI_WEATHER_PAGE_ID)) {
        system_log_write_info(TAG_TOPBAR, "weather page not registered");
    }
}

static void ui_pages_create_topbar(lv_obj_t *screen)
{
    const lv_coord_t topbar_h = APP_CONTENT_BOX_Y;

    s_topbar = lv_obj_create(screen);
    lv_obj_remove_style_all(s_topbar);
    lv_obj_set_size(s_topbar, APP_SCREEN_WIDTH, topbar_h);
    lv_obj_set_pos(s_topbar, 0, 0);
    lv_obj_clear_flag(s_topbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_topbar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_topbar, lv_color_hex(APP_UI_COLOR_TOPBAR_BG), LV_PART_MAIN);
    const theme_fx_t *fx = theme_fx_active();
    lv_obj_set_style_bg_opa(s_topbar, (fx != NULL) ? fx->bar_opa : LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_topbar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_topbar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_topbar, lv_color_hex(APP_UI_COLOR_TOPBAR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_topbar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_topbar, 0, LV_PART_MAIN);

    s_date_label = lv_label_create(s_topbar);
    lv_obj_set_width(s_date_label, 124);
    lv_obj_align(s_date_label, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(APP_UI_COLOR_TOPBAR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_date_label, TOPBAR_DATE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_date_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_text(s_date_label, "--.--.----");

    /* Settings gear - it sits in the right cluster, just before the HA chip. */
    s_gear_icon = lv_label_create(s_topbar);
    lv_obj_remove_style_all(s_gear_icon);
    lv_obj_set_size(s_gear_icon, 28, 28);
    lv_obj_align(s_gear_icon, LV_ALIGN_RIGHT_MID, -206, 0);
    lv_obj_add_flag(s_gear_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_gear_icon, 10);
    lv_obj_set_style_bg_color(s_gear_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_gear_icon, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_gear_icon, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_gear_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_gear_icon, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(s_gear_icon, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_gear_icon, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_gear_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_gear_icon, TOPBAR_ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_gear_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_gear_icon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_gear_icon, ui_topbar_gear_event_cb, LV_EVENT_CLICKED, NULL);
    ui_pages_set_topbar_icon(s_gear_icon, MDI_CP_COG, ui_i18n_get("topbar.settings", "SET"));

    /* Assist microphone, left of the gear (see ui_topbar_apply_layout). */
    s_assist_icon = lv_label_create(s_topbar);
    lv_obj_remove_style_all(s_assist_icon);
    lv_obj_set_size(s_assist_icon, 34, 28);
    lv_obj_add_flag(s_assist_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_assist_icon, 12);
    lv_obj_set_style_bg_color(s_assist_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_assist_icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_assist_icon, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_assist_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_assist_icon, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(s_assist_icon, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_assist_icon, 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_assist_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_assist_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_MUTED), LV_PART_MAIN);
    ui_assist_attach_chip(s_assist_icon);
    lv_obj_add_flag(s_assist_icon, LV_OBJ_FLAG_HIDDEN);

    /* Red alert triangle (left of the assist microphone when shown). */
    (void)ui_topbar_alerts_create(s_topbar, ui_topbar_apply_layout);

    /* Radio app shortcut, right next to the gear. It is only made visible once
     * a cameras page has been registered (see ui_topbar_apply_layout). */
    s_camera_icon = lv_label_create(s_topbar);
    lv_obj_remove_style_all(s_camera_icon);
    lv_obj_set_size(s_camera_icon, 28, 28);
    lv_obj_add_flag(s_camera_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_camera_icon, 10);
    lv_obj_set_style_bg_color(s_camera_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_camera_icon, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_camera_icon, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_camera_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_camera_icon, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(s_camera_icon, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_camera_icon, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_camera_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_camera_icon, TOPBAR_ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_camera_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_camera_icon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_camera_icon, ui_topbar_camera_event_cb, LV_EVENT_CLICKED, NULL);
    ui_pages_set_topbar_camera_icon(s_camera_icon);
    lv_obj_add_flag(s_camera_icon, LV_OBJ_FLAG_HIDDEN);

    /* Weather (Pogoda) shortcut, right after the cameras. Like the cameras chip it
     * only becomes visible once a "pogoda" page has been registered. */
    ui_topbar_waste_create(s_topbar, ui_topbar_apply_layout);

    s_weather_icon = lv_label_create(s_topbar);
    lv_obj_remove_style_all(s_weather_icon);
    lv_obj_set_size(s_weather_icon, 28, 28);
    lv_obj_align(s_weather_icon, LV_ALIGN_LEFT_MID, 142, 0);
    lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_weather_icon, 10);
    lv_obj_set_style_bg_color(s_weather_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_weather_icon, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_weather_icon, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_weather_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_weather_icon, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(s_weather_icon, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_weather_icon, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_weather_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_weather_icon, TOPBAR_ICON_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_weather_icon, lv_color_hex(APP_UI_COLOR_TOPBAR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_weather_icon, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_weather_icon, ui_topbar_weather_event_cb, LV_EVENT_CLICKED, NULL);
    ui_pages_set_topbar_icon(s_weather_icon, MDI_CP_WEATHER, ui_i18n_get("topbar.weather", "POGODA"));
    lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);

    s_time_label = lv_label_create(s_topbar);
    lv_obj_set_width(s_time_label, LV_SIZE_CONTENT);
    lv_obj_align(s_time_label, LV_ALIGN_RIGHT_MID, -208, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(APP_UI_COLOR_TOPBAR_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_time_label, TOPBAR_TIME_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_time_label, "--:--");

    s_api_icon = lv_label_create(s_topbar);
    lv_obj_set_width(s_api_icon, 86);
    lv_obj_align(s_api_icon, LV_ALIGN_RIGHT_MID, -158, 0);
    ui_pages_style_topbar_chip(s_api_icon);
    ui_pages_set_topbar_icon(s_api_icon, MDI_CP_HOME_ASSISTANT, ui_i18n_get("topbar.ha_off", "HA off"));

    s_wifi_icon = lv_label_create(s_topbar);
    lv_obj_set_width(s_wifi_icon, 96);
    lv_obj_align(s_wifi_icon, LV_ALIGN_RIGHT_MID, -56, 0);
    ui_pages_style_topbar_chip(s_wifi_icon);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_CLOSE);

    ui_pages_set_topbar_icon(s_wifi_icon, MDI_CP_WIFI_OFF, ui_i18n_get("topbar.wifi_off", "Wi-Fi off"));
    /* Colours, visibility and the final positions all come from the settings. */
    ui_topbar_load_settings();
}

static void ui_pages_create_nav(lv_obj_t *screen)
{
    const bool nav_left = ui_geometry_nav_left();
    s_nav_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(s_nav_bar);
    if (nav_left) {
        /* Vertical bar under the top bar, left of the content box. */
        lv_obj_set_size(s_nav_bar, APP_NAV_LEFT_WIDTH, APP_SCREEN_HEIGHT - APP_CONTENT_BOX_Y);
        lv_obj_set_pos(s_nav_bar, 0, APP_CONTENT_BOX_Y);
    } else {
        lv_obj_set_size(s_nav_bar, APP_SCREEN_WIDTH, APP_NAV_BAR_HEIGHT);
        lv_obj_set_pos(s_nav_bar, 0, APP_SCREEN_HEIGHT - APP_NAV_BAR_HEIGHT);
    }
    lv_obj_clear_flag(s_nav_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_nav_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_nav_bar, ui_nav_bar_bg_color(), LV_PART_MAIN | LV_STATE_DEFAULT);
    const theme_fx_t *fx = theme_fx_active();
    lv_obj_set_style_bg_opa(s_nav_bar, (fx != NULL) ? fx->bar_opa : LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* Same separators as the Guition panels: a 1 px top border drawn *inside*
     * the bar, and no shadow. The old floating shadow (16 px, offset -7) bled
     * ~10 px above the bar into the gutter under the tiles, which read as the
     * menu eating the bottom of the tile area. The content frame's bottom border
     * lands on this row as well, so the opaque bar still hides it. */
    lv_obj_set_style_border_width(s_nav_bar, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(
        s_nav_bar, nav_left ? LV_BORDER_SIDE_RIGHT : LV_BORDER_SIDE_TOP, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (nav_left) {
        /* More rooms than fit vertically: the bar scrolls. */
        lv_obj_add_flag(s_nav_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_nav_bar, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_nav_bar, LV_SCROLLBAR_MODE_OFF);
    }
    lv_obj_set_style_border_color(s_nav_bar, ui_nav_bar_border_color(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_nav_bar, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_nav_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_nav_bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_nav_home_button = lv_obj_create(s_nav_bar);
    lv_obj_remove_style_all(s_nav_home_button);
    lv_obj_set_ext_click_area(s_nav_home_button, 14);
    lv_obj_add_flag(s_nav_home_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_nav_home_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_nav_home_button, ui_nav_home_button_event_cb, LV_EVENT_CLICKED, NULL);

    s_nav_home_label = lv_label_create(s_nav_home_button);
    ui_pages_set_topbar_icon(s_nav_home_label, MDI_CP_HOME, LV_SYMBOL_HOME);
    lv_obj_center(s_nav_home_label);

    for (uint16_t i = 0; i < (APP_MAX_PAGES - 1); i++) {
        lv_obj_t *btn = lv_obj_create(s_nav_bar);
        lv_obj_remove_style_all(btn);
        lv_obj_set_ext_click_area(btn, 10);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, ui_nav_extra_button_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, "");
        lv_obj_set_style_text_font(label, NAV_TEXT_FONT, LV_PART_MAIN);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(label);

        s_nav_extra_buttons[i] = btn;
        s_nav_extra_labels[i] = label;
        s_nav_extra_page_index[i] = APP_MAX_PAGES;
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_pages_init(void)
{
    memset(s_pages, 0, sizeof(s_pages));
    s_page_count = 0;
    s_current_index = -1;
    s_background = NULL;
    s_topbar = NULL;
    s_content_box = NULL;
    s_date_label = NULL;
    s_time_label = NULL;
    s_wifi_icon = NULL;
    s_api_icon = NULL;
    s_gear_icon = NULL;
    s_camera_icon = NULL;
    s_weather_icon = NULL;
    s_assist_icon = NULL;
    ui_topbar_waste_reset();
    ui_topbar_alerts_reset();
    /* New labels: the next datetime update must write them even when the
     * minute has not changed since the last build. */
    s_topbar_date_text[0] = '\0';
    s_topbar_time_text[0] = '\0';
    s_nav_bar = NULL;
    s_nav_home_button = NULL;
    s_nav_home_label = NULL;
    memset(s_nav_extra_buttons, 0, sizeof(s_nav_extra_buttons));
    memset(s_nav_extra_labels, 0, sizeof(s_nav_extra_labels));
    memset(s_nav_extra_page_index, 0, sizeof(s_nav_extra_page_index));

#if LV_USE_LOTTIE && APP_UI_BETTA_LOTTIE_ASSET
    /* Screen will be cleaned below; the overlay is owned by the active screen.
     * Drop our handles and free the lottie render buffer to avoid a leak. */
    s_betta_overlay = NULL;
    s_betta_lottie  = NULL;
    if (s_betta_buf != NULL) {
        lv_free(s_betta_buf);
        s_betta_buf = NULL;
    }
    s_betta_taps    = 0U;
    s_betta_last_ms = 0U;
#endif

    lv_obj_t *screen = lv_scr_act();
    /* The lv_obj_clean() below destroys every child of the active screen,
     * including overlays other modules created on it. Let them drop their
     * handles first, otherwise their timers keep using freed objects. */
    ui_screen_saver_handle_screen_clean();
    lv_obj_clean(screen);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(APP_UI_COLOR_SCREEN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);

    s_background = lv_obj_create(screen);
    lv_obj_remove_style_all(s_background);
    lv_obj_set_size(s_background, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
    lv_obj_set_pos(s_background, 0, 0);
    lv_obj_clear_flag(s_background, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_background, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_background, lv_color_hex(APP_UI_COLOR_SCREEN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_background, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_background, 0, LV_PART_MAIN);
    /* Effect themes paint a generated backdrop behind everything; the content
     * box and both bars are then (partly) see-through. */
    const theme_fx_t *fx = theme_fx_active();
    const lv_image_dsc_t *backdrop = theme_fx_backdrop();
    if (backdrop != NULL) {
        lv_obj_set_style_bg_image_src(s_background, backdrop, LV_PART_MAIN);
        lv_obj_set_style_bg_image_opa(s_background, LV_OPA_COVER, LV_PART_MAIN);
    }

    s_content_box = lv_obj_create(screen);
    lv_obj_remove_style_all(s_content_box);
    /* A border insets the parent's content area, and LVGL clips children to it.
     * With the frame below and the old 1024x480 box every page (and therefore
     * every tile) was drawn one pixel down/right and the tiles' bottom and
     * right outlines fell outside the clip - the tiles looked cut off by the
     * navigation bar. Grow the box by the frame so the *content* area is again
     * exactly APP_CONTENT_BOX_* at (APP_CONTENT_BOX_X, APP_CONTENT_BOX_Y); the
     * frame itself then lands under the top bar / nav bar / off the right edge. */
#if APP_UI_REWORK_V2
    const lv_coord_t content_frame = 1;
#else
    const lv_coord_t content_frame = 0;
#endif
    lv_obj_set_size(s_content_box, APP_CONTENT_BOX_WIDTH + (2 * content_frame),
                    APP_CONTENT_BOX_HEIGHT + (2 * content_frame));
    lv_obj_set_pos(s_content_box, APP_CONTENT_BOX_X - content_frame, APP_CONTENT_BOX_Y - content_frame);
    lv_obj_clear_flag(s_content_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_content_box, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_content_box, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_content_box, (backdrop != NULL) ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_content_box, content_frame, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_content_box, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_content_box, (fx != NULL) ? LV_OPA_TRANSP : LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_content_box, 0, LV_PART_MAIN);

    ui_pages_create_topbar(screen);
    ui_pages_create_nav(screen);

    time_t now = time(NULL);
    struct tm info = {0};
    localtime_r(&now, &info);
    ui_pages_set_topbar_datetime(&info);
    ui_pages_set_topbar_status(false, false, false, false);
    ui_pages_apply_tab_style(0);

    if (s_screen_built_cb != NULL) {
        s_screen_built_cb();
    }
}

void ui_pages_reset(void)
{
    s_camera_page_id[0] = '\0';
    ui_pages_init();
}

void ui_pages_set_camera_shortcut(const char *page_id)
{
    /* The first cameras page of the layout owns the top bar shortcut. */
    if (page_id == NULL || page_id[0] == '\0' || s_camera_page_id[0] != '\0') {
        return;
    }
    snprintf(s_camera_page_id, sizeof(s_camera_page_id), "%s", page_id);
}

lv_obj_t *ui_pages_add_ex(const char *page_id, const char *title, bool in_nav)
{
    if (s_page_count >= APP_MAX_PAGES || page_id == NULL || page_id[0] == '\0' || s_content_box == NULL) {
        return NULL;
    }

    uint16_t index = s_page_count;
    snprintf(s_pages[index].id, sizeof(s_pages[index].id), "%s", page_id);
    snprintf(s_pages[index].title, sizeof(s_pages[index].title), "%s", (title && title[0]) ? title : page_id);
    s_pages[index].in_nav = in_nav;

    lv_obj_t *container = lv_obj_create(s_content_box);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, APP_CONTENT_BOX_WIDTH, APP_CONTENT_BOX_HEIGHT);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    s_pages[index].container = container;

    s_page_count++;
    ui_pages_apply_tab_style((uint16_t)(s_current_index >= 0 ? s_current_index : 0));
    return container;
}

lv_obj_t *ui_pages_add(const char *page_id, const char *title)
{
    return ui_pages_add_ex(page_id, title, true);
}

lv_obj_t *ui_pages_add_hidden(const char *page_id, const char *title)
{
    return ui_pages_add_ex(page_id, title, false);
}

bool ui_pages_show_index(uint16_t index)
{
    if (index >= s_page_count) {
        return false;
    }

    for (uint16_t i = 0; i < s_page_count; i++) {
        if (s_pages[i].container == NULL) {
            continue;
        }
        if (i == index) {
            lv_obj_clear_flag(s_pages[i].container, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* Leftover animation from a previous switch would keep moving an
             * invisible page; drop it before hiding. */
            ui_page_transition_reset(s_pages[i].container);
            lv_obj_add_flag(s_pages[i].container, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ui_page_transition_run(s_pages[index].container, s_current_index, (int)index);
    s_current_index = (int16_t)index;
    ui_pages_apply_tab_style(index);
    /* Logged at INFO *and* noted for the flash detector: a page switch is one of
     * the few events that repaints the whole screen, so a flash reported right
     * afterwards can be attributed to it. */
    ESP_LOGI(TAG_PAGES, "page %u/%u -> %s", (unsigned)(index + 1U), (unsigned)s_page_count, s_pages[index].id);
    {
        char note[APP_MAX_PAGE_ID_LEN + 8];
        snprintf(note, sizeof(note), "page:%s", s_pages[index].id);
        display_flash_watch_note(note);
    }
    system_log_event("ui", "page -> %s", s_pages[index].id);
    if (strncmp(s_pages[index].id, "energy", 6) == 0) {
        ha_client_energy_page_shown();
    }
    if (s_show_cb != NULL) {
        s_show_cb(s_pages[index].id, index);
    }
    return true;
}

bool ui_pages_show(const char *page_id)
{
    const int16_t index = ui_pages_find(page_id);
    if (index < 0) {
        return false;
    }
    return ui_pages_show_index((uint16_t)index);
}

bool ui_pages_next(void)
{
    if (s_page_count == 0) {
        return false;
    }
    uint16_t next = (uint16_t)(((s_current_index < 0 ? 0 : s_current_index) + 1) % s_page_count);
    return ui_pages_show_index(next);
}

const char *ui_pages_current_id(void)
{
    if (s_current_index < 0 || (uint16_t)s_current_index >= s_page_count) {
        return "";
    }
    return s_pages[s_current_index].id;
}

const char *ui_pages_id_at(uint16_t index)
{
    if (index >= s_page_count) {
        return "";
    }
    return s_pages[index].id;
}

const char *ui_pages_title_at(uint16_t index)
{
    if (index >= s_page_count) {
        return "";
    }
    return s_pages[index].title;
}

uint16_t ui_pages_count(void)
{
    return s_page_count;
}

void ui_pages_set_topbar_status(
    bool wifi_connected, bool wifi_setup_ap_active, bool api_connected, bool api_initial_sync_done)
{
    s_status_wifi = wifi_connected;
    s_status_wifi_ap = wifi_setup_ap_active;
    s_status_ha = api_connected;
    s_status_ha_sync = api_initial_sync_done;

    if (!display_lock(TOPBAR_LOCK_TIMEOUT_MS)) {
        return;
    }
    ui_topbar_render_status();
    ui_topbar_apply_layout();
    display_unlock();
}

/* Date in the configured format: WD = weekday, DD, MM, YYYY; a format without
 * the year ends with a dot when it uses dots ("24.10."). */
static void ui_topbar_format_date(char *out, size_t out_len, const struct tm *t, const char *weekday)
{
    const char *fmt = s_topbar_cfg.date_format[0] != '\0' ? s_topbar_cfg.date_format : "WD, DD.MM.YYYY";
    size_t n = 0;
    bool has_year = false;
    bool dotted = false;
    out[0] = '\0';
    for (const char *p = fmt; *p != '\0' && n + 1 < out_len;) {
        int w = 0;
        if (strncmp(p, "WD", 2) == 0) {
            w = snprintf(out + n, out_len - n, "%s", weekday);
            p += 2;
        } else if (strncmp(p, "YYYY", 4) == 0) {
            w = snprintf(out + n, out_len - n, "%04d", t->tm_year + 1900);
            has_year = true;
            p += 4;
        } else if (strncmp(p, "DD", 2) == 0) {
            w = snprintf(out + n, out_len - n, "%02d", t->tm_mday);
            p += 2;
        } else if (strncmp(p, "MM", 2) == 0) {
            w = snprintf(out + n, out_len - n, "%02d", t->tm_mon + 1);
            p += 2;
        } else {
            if (*p == '.') {
                dotted = true;
            }
            out[n] = *p++;
            out[n + 1] = '\0';
            w = 1;
        }
        if (w > 0) {
            n += (size_t)w;
        }
        if (n >= out_len) {
            n = out_len - 1;
        }
    }
    if (!has_year && dotted && n + 1 < out_len && n > 0 && out[n - 1] != '.') {
        out[n++] = '.';
        out[n] = '\0';
    }
}

void ui_pages_set_topbar_datetime(const struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return;
    }

    static const struct {
        const char *key;
        const char *fallback;
    } WEEKDAYS[7] = {
        {"weekday.su", "Su"}, {"weekday.mo", "Mo"}, {"weekday.tu", "Tu"}, {"weekday.we", "We"},
        {"weekday.th", "Th"}, {"weekday.fr", "Fr"}, {"weekday.sa", "Sa"},
    };
    const int wday = (timeinfo->tm_wday >= 0 && timeinfo->tm_wday <= 6) ? timeinfo->tm_wday : 0;

    char date_buf[40] = {0};
    char time_buf[32] = {0};
    ui_topbar_format_date(date_buf, sizeof(date_buf), timeinfo,
        ui_i18n_get(WEEKDAYS[wday].key, WEEKDAYS[wday].fallback));
    s_topbar_time = *timeinfo;
    s_topbar_have_time = true;
    ui_topbar_clock_text(time_buf, sizeof(time_buf));

    const bool changed = strcmp(date_buf, s_topbar_date_text) != 0 ||
        strcmp(time_buf, s_topbar_time_text) != 0;

    snprintf(s_topbar_date_text, sizeof(s_topbar_date_text), "%s", date_buf);
    snprintf(s_topbar_time_text, sizeof(s_topbar_time_text), "%s", time_buf);

    /* LVGL's lv_label_set_text() is not idempotent: it frees and reallocates the
     * label's text buffer and invalidates the widget even when the string is
     * identical.  This runs once per second, so re-writing the same minute and
     * the same date repainted the whole top bar and churned the (already badly
     * fragmented) heap for nothing - one of the things that can be seen as the
     * bar blinking.  The labels only need the lock when their text really
     * changed, or when one of them does not exist yet. */
    if (!changed && s_date_label != NULL && s_time_label != NULL) {
        return;
    }

    if (!display_lock(TOPBAR_LOCK_TIMEOUT_MS)) {
        return;
    }
    if (s_date_label != NULL) {
        lv_label_set_text(s_date_label, date_buf);
    }
    if (s_time_label != NULL) {
        lv_label_set_text(s_time_label, time_buf);
    }
    if (changed) {
        ui_topbar_apply_layout();
    }
    display_unlock();
}