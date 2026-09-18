/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_music_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ha/ha_cover_fetcher.h"
#include "ha/ha_model.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_bindings.h"
#include "ui/ui_memory.h"
#include "ui/ui_slider_touch.h"
#include "diag/system_log.h"

#define TAG "ui_music"

/*
 * Full-screen "Now Playing" view for a Music Assistant player.
 *
 * The page owns exactly one media_player entity at a time (the "current
 * player"). A tap on the player chip cycles through the configured players.
 * Album art is fetched through the global cover fetcher and its dominant
 * colour is used to accent the controls, so the whole page follows the mood
 * of the currently playing album.
 */

/* ---- Geometry ----------------------------------------------------------- */
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
#define MUSIC_HERO_X 8
#define MUSIC_HERO_Y 6
#define MUSIC_HERO_W (APP_CONTENT_BOX_WIDTH - 16)
#define MUSIC_HERO_H (APP_CONTENT_BOX_HEIGHT - 12)
#define MUSIC_HERO_RADIUS 26

#define MUSIC_CHIP_X 22
#define MUSIC_CHIP_Y 16
#define MUSIC_CHIP_W 176
#define MUSIC_CHIP_H 34
#define MUSIC_BADGE_X 350
#define MUSIC_BADGE_Y 19
#define MUSIC_BADGE_W 108
#define MUSIC_BADGE_H 28

#define MUSIC_BTN_TAB_Y 16
#define MUSIC_BTN_TAB_H 34
#define MUSIC_BTN_LIST_X 204
#define MUSIC_BTN_LIST_W 48
#define MUSIC_BTN_SEARCH_X 258
#define MUSIC_BTN_SEARCH_W 58
#define MUSIC_BTN_TAB_FONT APP_FONT_TEXT_14

#define MUSIC_COVER_X 22
#define MUSIC_COVER_Y 58
#define MUSIC_COVER_SIZE 150

#define MUSIC_INFO_X 186
#define MUSIC_INFO_W 270
#define MUSIC_TITLE_Y 60
#define MUSIC_TITLE_H 64
#define MUSIC_ARTIST_Y 130
#define MUSIC_ARTIST_H 24

#define MUSIC_PROG_X 22
#define MUSIC_PROG_Y 220
#define MUSIC_PROG_W 430
#define MUSIC_PROG_H 8
#define MUSIC_POS_X 22
#define MUSIC_POS_W 120
#define MUSIC_DUR_X 332
#define MUSIC_DUR_W 120
#define MUSIC_TIME_Y 232

#define MUSIC_TRANSPORT_Y 252
#define MUSIC_BTN_PREV_X 150
#define MUSIC_BTN_PLAY_X 208
#define MUSIC_BTN_NEXT_X 288
#define MUSIC_BTN_SMALL 52
#define MUSIC_BTN_PLAY_SIZE 68

#define MUSIC_VOL_ICON_X 22
#define MUSIC_VOL_ICON_Y 330
#define MUSIC_VOL_SLIDER_X 44
#define MUSIC_VOL_SLIDER_Y 326
#define MUSIC_VOL_SLIDER_W 406
#define MUSIC_VOL_SLIDER_H 16

#define MUSIC_TITLE_FONT APP_FONT_TEXT_20
#define MUSIC_ARTIST_FONT APP_FONT_TEXT_16
#define MUSIC_BADGE_FONT APP_FONT_TEXT_14
#define MUSIC_TIME_FONT APP_FONT_TEXT_14
#define MUSIC_CHIP_FONT APP_FONT_TEXT_16
#define MUSIC_PLAY_FONT APP_FONT_DISPLAY_40
#define MUSIC_NAV_FONT APP_FONT_TEXT_24
#define MUSIC_VOL_ICON_FONT APP_FONT_TEXT_16
#define MUSIC_COVER_TARGET 128
#else
#define MUSIC_HERO_X 16
#define MUSIC_HERO_Y 12
#define MUSIC_HERO_W (APP_CONTENT_BOX_WIDTH - 32)
#define MUSIC_HERO_H (APP_CONTENT_BOX_HEIGHT - 24)
#define MUSIC_HERO_RADIUS 30

#define MUSIC_CHIP_X 32
#define MUSIC_CHIP_Y 28
#define MUSIC_CHIP_W 320
#define MUSIC_CHIP_H 48
#define MUSIC_BADGE_X (MUSIC_HERO_W - 172)
#define MUSIC_BADGE_Y 33
#define MUSIC_BADGE_W 150
#define MUSIC_BADGE_H 38

#define MUSIC_BTN_TAB_Y 28
#define MUSIC_BTN_TAB_H 48
#define MUSIC_BTN_LIST_X 356
#define MUSIC_BTN_LIST_W 72
#define MUSIC_BTN_SEARCH_X 436
#define MUSIC_BTN_SEARCH_W 72
#define MUSIC_BTN_TAB_FONT APP_FONT_TEXT_16

#define MUSIC_COVER_X 32
#define MUSIC_COVER_Y 92
#define MUSIC_COVER_SIZE 220

#define MUSIC_INFO_X 274
#define MUSIC_INFO_W (MUSIC_HERO_W - MUSIC_INFO_X - 32)
#define MUSIC_TITLE_Y 96
#define MUSIC_TITLE_H 92
#define MUSIC_ARTIST_Y 198
#define MUSIC_ARTIST_H 34

#define MUSIC_PROG_X 32
#define MUSIC_PROG_Y 336
#define MUSIC_PROG_W (MUSIC_HERO_W - 64)
#define MUSIC_PROG_H 12
#define MUSIC_POS_X 32
#define MUSIC_POS_W 160
#define MUSIC_DUR_X (MUSIC_HERO_W - 192)
#define MUSIC_DUR_W 160
#define MUSIC_TIME_Y 356

#define MUSIC_TRANSPORT_Y 388
#define MUSIC_BTN_PREV_X 210
#define MUSIC_BTN_PLAY_X 300
#define MUSIC_BTN_NEXT_X 420
#define MUSIC_BTN_SMALL 76
#define MUSIC_BTN_PLAY_SIZE 100

#define MUSIC_VOL_ICON_X 32
#define MUSIC_VOL_ICON_Y 520
#define MUSIC_VOL_SLIDER_X 72
#define MUSIC_VOL_SLIDER_Y 510
#define MUSIC_VOL_SLIDER_W (MUSIC_HERO_W - 112)
#define MUSIC_VOL_SLIDER_H 24

#define MUSIC_TITLE_FONT APP_FONT_TEXT_28
#define MUSIC_ARTIST_FONT APP_FONT_TEXT_22
#define MUSIC_BADGE_FONT APP_FONT_TEXT_18
#define MUSIC_TIME_FONT APP_FONT_TEXT_18
#define MUSIC_CHIP_FONT APP_FONT_TEXT_22
#define MUSIC_PLAY_FONT APP_FONT_DISPLAY_40
#define MUSIC_NAV_FONT APP_FONT_DISPLAY_28
#define MUSIC_VOL_ICON_FONT APP_FONT_TEXT_22
#define MUSIC_COVER_TARGET 220
#endif

/* ---- Library overlay geometry (proportional to the content box) -------- */
#define MUSIC_LIB_PAD 6
#define MUSIC_LIB_HEADER_H 48
#define MUSIC_LIB_BACK_SIZE 36
#define MUSIC_LIB_TITLE_X (MUSIC_LIB_PAD + MUSIC_LIB_BACK_SIZE + 8)
#define MUSIC_LIB_TITLE_W (APP_CONTENT_BOX_WIDTH - MUSIC_LIB_TITLE_X - MUSIC_LIB_PAD)
#define MUSIC_LIB_LIST_X MUSIC_LIB_PAD
#define MUSIC_LIB_LIST_W (APP_CONTENT_BOX_WIDTH - 2 * MUSIC_LIB_PAD)
#define MUSIC_LIB_LIST_Y MUSIC_LIB_HEADER_H
#define MUSIC_LIB_LIST_H (APP_CONTENT_BOX_HEIGHT - MUSIC_LIB_HEADER_H - MUSIC_LIB_PAD)
#define MUSIC_LIB_SEARCH_H 40
#define MUSIC_LIB_SEARCH_BTN_W 64
#define MUSIC_LIB_SEARCH_W (APP_CONTENT_BOX_WIDTH - 2 * MUSIC_LIB_PAD - MUSIC_LIB_SEARCH_BTN_W - 8)
#define MUSIC_LIB_SEARCH_X MUSIC_LIB_PAD
#define MUSIC_LIB_SEARCH_Y MUSIC_LIB_HEADER_H
#define MUSIC_LIB_SEARCH_BTN_X (MUSIC_LIB_SEARCH_X + MUSIC_LIB_SEARCH_W + 8)
#define MUSIC_LIB_KEYBOARD_H 150
#define MUSIC_LIB_KEYBOARD_Y (APP_CONTENT_BOX_HEIGHT - MUSIC_LIB_KEYBOARD_H)
#define MUSIC_LIB_SEARCH_LIST_Y (MUSIC_LIB_SEARCH_Y + MUSIC_LIB_SEARCH_H + 8)
#define MUSIC_LIB_SEARCH_LIST_H (APP_CONTENT_BOX_HEIGHT - MUSIC_LIB_SEARCH_LIST_Y - MUSIC_LIB_KEYBOARD_H - MUSIC_LIB_PAD)

#define MUSIC_MAX_TITLE_LEN 192
#define MUSIC_MAX_COVER_URL_LEN 512
#define MUSIC_TICK_PERIOD_MS 500

#define MUSIC_LIB_MAX_ITEMS 48
#define MUSIC_LIB_TITLE_MAX 160
#define MUSIC_LIB_CLASS_MAX 32
#define MUSIC_LIB_CTYPE_MAX 40
#define MUSIC_LIB_ID_MAX 200

typedef struct {
    char title[MUSIC_LIB_TITLE_MAX];
    char media_class[MUSIC_LIB_CLASS_MAX];
    char media_content_type[MUSIC_LIB_CTYPE_MAX];
    char media_content_id[MUSIC_LIB_ID_MAX];
    bool can_play;
} music_lib_item_t;

typedef struct music_lib_result music_lib_result_t;

typedef struct {
    ui_music_page_config_t config;
    int current_player_index;

    lv_obj_t *root;
    lv_obj_t *hero;
    lv_obj_t *player_chip;
    lv_obj_t *player_chip_label;
    lv_obj_t *state_badge;
    lv_obj_t *cover_img;
    lv_obj_t *cover_placeholder;
    lv_obj_t *title_label;
    lv_obj_t *artist_label;
    lv_obj_t *progress_bar;
    lv_obj_t *pos_label;
    lv_obj_t *dur_label;
    lv_obj_t *btn_prev;
    lv_obj_t *btn_prev_label;
    lv_obj_t *btn_play;
    lv_obj_t *btn_play_label;
    lv_obj_t *btn_next;
    lv_obj_t *btn_next_label;
    lv_obj_t *vol_icon;
    lv_obj_t *vol_slider;
    lv_timer_t *tick_timer;

    /* Current media state */
    char now_title[MUSIC_MAX_TITLE_LEN];
    char now_artist[MUSIC_MAX_TITLE_LEN];
    char cover_url[MUSIC_MAX_COVER_URL_LEN];
    int duration_s;
    int position_s;
    time_t position_anchor_s;
    bool is_playing;
    bool unavailable;
    int volume_pct;
    bool volume_muted;
    bool volume_dragging;

    /* Cover art */
    lv_image_dsc_t cover_dsc;
    uint32_t cover_dominant_rgb;
    bool cover_request_inflight;
    int cover_fail_count;
    int64_t cover_retry_after_ms;

    /* Music Assistant library browsing (playlists + search) */
    lv_obj_t *lib_overlay;
    lv_obj_t *lib_title_label;
    lv_obj_t *lib_list;
    lv_obj_t *lib_back_btn;
    lv_obj_t *search_row;
    lv_obj_t *search_textarea;
    lv_obj_t *search_btn;
    lv_obj_t *keyboard;
    lv_obj_t *btn_playlists;
    lv_obj_t *btn_playlists_label;
    lv_obj_t *btn_search;
    lv_obj_t *btn_search_label;
    int lib_mode;
    int lib_req_gen;
    music_lib_result_t *lib_result;
} music_ctx_t;

typedef struct music_lib_result {
    music_ctx_t *ctx;
    int gen;
    bool ok;
    int count;
    music_lib_item_t items[];
} music_lib_result_t;

/* Guard used to drop an already-enqueued lv_async_call if the page is
 * destroyed before the callback runs (mirrors the cover fetcher pattern). */
static music_ctx_t *s_live_music_ctx = NULL;

enum {
    MP_LIB_MODE_NONE = 0,
    MP_LIB_MODE_PLAYLISTS,
    MP_LIB_MODE_SEARCH,
};

/* ---- Small helpers ------------------------------------------------------ */

static int mp_clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void mp_format_time(int seconds, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) return;
    if (seconds < 0) seconds = 0;
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0) {
        snprintf(out, out_sz, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(out, out_sz, "%d:%02d", m, s);
    }
}

static bool mp_parse_iso8601_utc(const char *s, time_t *out)
{
    if (s == NULL || out == NULL) return false;
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    int consumed = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d%n", &year, &mon, &day, &hour, &min, &sec, &consumed) < 6) {
        return false;
    }
    const char *p = s + consumed;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++;
    }
    int tz_offset_s = 0;
    if (*p == '+' || *p == '-') {
        int sign = (*p == '+') ? 1 : -1;
        p++;
        int tz_h = 0, tz_m = 0;
        if (sscanf(p, "%2d:%2d", &tz_h, &tz_m) == 2 || sscanf(p, "%2d%2d", &tz_h, &tz_m) == 2) {
            tz_offset_s = sign * (tz_h * 3600 + tz_m * 60);
        }
    }

    int y = year;
    int m = mon;
    if (m <= 2) { y -= 1; m += 12; }
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153U * (unsigned)(m - 3) + 2U) / 5U + (unsigned)day - 1U;
    unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    long days = era * 146097L + (long)doe - 719468L;
    time_t t = (time_t)(days * 86400L + hour * 3600L + min * 60L + sec - tz_offset_s);
    *out = t;
    return true;
}

static lv_color_t mp_pressed_variant(lv_color_t color)
{
    return lv_color_brightness(color) > 127 ? lv_color_darken(color, 28) : lv_color_lighten(color, 28);
}

static lv_color_t mp_contrast_text(lv_color_t bg)
{
    return lv_color_brightness(bg) > 150 ? lv_color_hex(0x101418) : lv_color_hex(0xFFFFFF);
}

static lv_color_t mp_tint_hero(uint32_t dominant_rgb)
{
    return lv_color_mix(lv_color_hex(dominant_rgb & 0xFFFFFFU), lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), 56);
}

static lv_color_t mp_accent(const music_ctx_t *ctx)
{
    if (ctx->unavailable) {
        return lv_color_hex(APP_UI_COLOR_CARD_BORDER);
    }
    if (ctx->cover_dominant_rgb != 0) {
        return lv_color_mix(lv_color_hex(ctx->cover_dominant_rgb & 0xFFFFFFU),
            lv_color_hex(APP_UI_COLOR_CARD_ICON_ON), 80);
    }
    return lv_color_hex(APP_UI_COLOR_CARD_ICON_ON);
}

static bool mp_state_unavailable(const char *state)
{
    if (state == NULL) return true;
    return strcmp(state, "unavailable") == 0 || strcmp(state, "unknown") == 0;
}

static int64_t mp_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int mp_current_position_s(const music_ctx_t *ctx)
{
    if (ctx == NULL) return 0;
    int pos = ctx->position_s;
    if (ctx->is_playing && !ctx->unavailable && ctx->position_anchor_s > 0) {
        time_t now_s = time(NULL);
        if (now_s > ctx->position_anchor_s) {
            long elapsed = (long)(now_s - ctx->position_anchor_s);
            if (elapsed > 0 && elapsed < 24 * 3600) {
                pos += (int)elapsed;
            }
        }
    }
    if (ctx->duration_s > 0 && pos > ctx->duration_s) {
        pos = ctx->duration_s;
    }
    if (pos < 0) pos = 0;
    return pos;
}

/* ---- Visual restyle ----------------------------------------------------- */

static void mp_apply_visual(music_ctx_t *ctx)
{
    if (ctx == NULL || ctx->hero == NULL) return;

    lv_color_t accent = mp_accent(ctx);
    lv_color_t text_primary = lv_color_hex(ctx->unavailable ? APP_UI_COLOR_TEXT_MUTED : APP_UI_COLOR_TEXT_PRIMARY);
    lv_color_t text_muted = lv_color_hex(APP_UI_COLOR_TEXT_MUTED);
    lv_color_t control_surface = lv_color_mix(lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE),
        lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), 224);
    lv_color_t control_border = lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER);
    lv_color_t play_bg = ctx->unavailable
                             ? control_surface
                             : lv_color_mix(accent, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE), 224);
    lv_color_t play_border = ctx->unavailable ? control_border : lv_color_mix(accent, control_border, 168);
    lv_color_t play_text = ctx->unavailable ? text_muted : mp_contrast_text(play_bg);

    /* Hero background follows the album mood once cover art has arrived. */
    if (!ctx->unavailable && ctx->cover_dominant_rgb != 0) {
        lv_obj_set_style_bg_color(ctx->hero, mp_tint_hero(ctx->cover_dominant_rgb), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(ctx->hero, lv_color_hex(ctx->unavailable ? APP_UI_COLOR_CARD_BG_OFF
                                                                          : APP_UI_COLOR_CARD_BG_ON),
            LV_PART_MAIN);
    }

    if (ctx->title_label) {
        lv_obj_set_style_text_color(ctx->title_label, text_primary, LV_PART_MAIN);
    }
    if (ctx->artist_label) {
        lv_obj_set_style_text_color(ctx->artist_label, text_muted, LV_PART_MAIN);
    }
    if (ctx->pos_label) {
        lv_obj_set_style_text_color(ctx->pos_label, text_muted, LV_PART_MAIN);
    }
    if (ctx->dur_label) {
        lv_obj_set_style_text_color(ctx->dur_label, text_muted, LV_PART_MAIN);
    }
    if (ctx->vol_icon) {
        lv_obj_set_style_text_color(ctx->vol_icon, text_muted, LV_PART_MAIN);
    }
    if (ctx->state_badge) {
        lv_obj_set_style_bg_color(ctx->state_badge,
            ctx->unavailable ? control_surface : lv_color_mix(accent, control_surface, 96), LV_PART_MAIN);
        lv_obj_set_style_text_color(ctx->state_badge, ctx->unavailable ? text_muted : mp_contrast_text(accent),
            LV_PART_MAIN);
    }
    if (ctx->player_chip) {
        lv_obj_set_style_bg_color(ctx->player_chip, control_surface, LV_PART_MAIN);
        lv_obj_set_style_text_color(ctx->player_chip_label, text_primary, LV_PART_MAIN);
    }

    if (ctx->progress_bar) {
        lv_obj_set_style_bg_color(ctx->progress_bar, control_surface, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ctx->progress_bar, accent, LV_PART_INDICATOR);
    }
    if (ctx->vol_slider) {
        lv_obj_set_style_bg_color(ctx->vol_slider, control_surface, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ctx->vol_slider, accent, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(ctx->vol_slider,
            ctx->unavailable ? text_muted : lv_color_mix(lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), accent, 72),
            LV_PART_KNOB);
    }

    if (ctx->btn_play_label) {
        lv_label_set_text(ctx->btn_play_label, ctx->is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    /* Control buttons */
    lv_color_t pressed = mp_pressed_variant(control_surface);
    lv_obj_t *small_btns[2] = { ctx->btn_prev, ctx->btn_next };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = small_btns[i];
        if (btn == NULL) continue;
        lv_obj_set_style_bg_color(btn, control_surface, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, pressed, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, control_border, LV_PART_MAIN);
        lv_obj_set_style_text_color(i == 0 ? ctx->btn_prev_label : ctx->btn_next_label, text_primary, LV_PART_MAIN);
    }
    if (ctx->btn_play) {
        lv_obj_set_style_bg_color(ctx->btn_play, play_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ctx->btn_play, mp_pressed_variant(play_bg), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_color(ctx->btn_play, play_border, LV_PART_MAIN);
        lv_obj_set_style_text_color(ctx->btn_play_label, play_text, LV_PART_MAIN);
    }

    /* Library tab buttons */
    if (ctx->btn_playlists_label) {
        lv_obj_set_style_text_color(ctx->btn_playlists_label,
            ctx->unavailable ? text_muted
                             : (ctx->lib_mode == MP_LIB_MODE_PLAYLISTS ? accent : text_primary),
            LV_PART_MAIN);
    }
    if (ctx->btn_search_label) {
        lv_obj_set_style_text_color(ctx->btn_search_label,
            ctx->unavailable ? text_muted : (ctx->lib_mode == MP_LIB_MODE_SEARCH ? accent : text_primary),
            LV_PART_MAIN);
    }

    const lv_opa_t control_opa = ctx->unavailable ? LV_OPA_30 : LV_OPA_COVER;
    lv_obj_set_style_opa(ctx->btn_prev, control_opa, LV_PART_MAIN);
    lv_obj_set_style_opa(ctx->btn_play, control_opa, LV_PART_MAIN);
    lv_obj_set_style_opa(ctx->btn_next, control_opa, LV_PART_MAIN);
}

static void mp_update_progress(music_ctx_t *ctx)
{
    if (ctx == NULL) return;
    int pos = mp_current_position_s(ctx);
    int dur = ctx->duration_s;
    if (ctx->progress_bar != NULL) {
        if (dur > 0) {
            int pct = (int)((int64_t)pos * 100 / dur);
            pct = mp_clamp_int(pct, 0, 100);
            lv_bar_set_value(ctx->progress_bar, pct, LV_ANIM_OFF);
        } else {
            lv_bar_set_value(ctx->progress_bar, 0, LV_ANIM_OFF);
        }
    }
    if (ctx->pos_label != NULL) {
        char buf[16];
        mp_format_time(pos, buf, sizeof(buf));
        lv_label_set_text(ctx->pos_label, buf);
    }
    if (ctx->dur_label != NULL) {
        char buf[16];
        if (dur > 0) {
            mp_format_time(dur, buf, sizeof(buf));
        } else {
            snprintf(buf, sizeof(buf), "--:--");
        }
        lv_label_set_text(ctx->dur_label, buf);
    }
}

static void mp_update_volume_visual(music_ctx_t *ctx)
{
    if (ctx == NULL) return;
    int pct = ctx->volume_muted ? 0 : mp_clamp_int(ctx->volume_pct, 0, 100);
    if (ctx->vol_slider != NULL && !ctx->volume_dragging) {
        lv_slider_set_value(ctx->vol_slider, pct, LV_ANIM_OFF);
    }
    if (ctx->vol_icon != NULL) {
        lv_label_set_text(ctx->vol_icon, (ctx->volume_muted || pct == 0) ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MID);
    }
}

/* ---- Cover art ---------------------------------------------------------- */

static void mp_release_cover(music_ctx_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->cover_dsc.data != NULL) {
        heap_caps_free((void *)ctx->cover_dsc.data);
        ctx->cover_dsc.data = NULL;
        ctx->cover_dsc.data_size = 0;
    }
}

static void mp_cover_show_placeholder(music_ctx_t *ctx)
{
    if (ctx == NULL) return;
    ctx->cover_dominant_rgb = 0;
    if (ctx->cover_img != NULL) {
        lv_image_set_src(ctx->cover_img, NULL);
        lv_obj_set_style_bg_color(ctx->cover_img, lv_color_mix(lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE),
            lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), 184), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ctx->cover_img, LV_OPA_COVER, LV_PART_MAIN);
    }
    if (ctx->cover_placeholder != NULL) {
        lv_obj_clear_flag(ctx->cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mp_note_cover_failure(music_ctx_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->cover_fail_count < 6) {
        ctx->cover_fail_count++;
    }
    int64_t backoff_ms = 5000LL * (int64_t)ctx->cover_fail_count;
    if (backoff_ms > 60000LL) {
        backoff_ms = 60000LL;
    }
    ctx->cover_retry_after_ms = mp_now_ms() + backoff_ms;
}

static void mp_cover_cb(void *user, const ha_cover_result_t *result)
{
    music_ctx_t *ctx = (music_ctx_t *)user;
    if (ctx == NULL) return;
    ctx->cover_request_inflight = false;
    if (result == NULL || !result->valid) {
        mp_note_cover_failure(ctx);
        mp_cover_show_placeholder(ctx);
        mp_apply_visual(ctx);
        return;
    }

    mp_release_cover(ctx);
    ctx->cover_dsc = result->image;
    ctx->cover_dominant_rgb = result->dominant_rgb;
    ctx->cover_fail_count = 0;
    ctx->cover_retry_after_ms = 0;

    if (ctx->cover_img != NULL) {
        lv_image_set_src(ctx->cover_img, &ctx->cover_dsc);
        lv_coord_t tgt_w = lv_obj_get_width(ctx->cover_img);
        lv_coord_t tgt_h = lv_obj_get_height(ctx->cover_img);
        if (tgt_w > 0 && ctx->cover_dsc.header.w > 0) {
            int32_t zoom_w = (int32_t)((int64_t)tgt_w * 256 / ctx->cover_dsc.header.w);
            int32_t zoom_h = (int32_t)((int64_t)tgt_h * 256 / ctx->cover_dsc.header.h);
            int32_t zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
            if (zoom < 16) zoom = 16;
            lv_image_set_scale(ctx->cover_img, (uint16_t)zoom);
        }
        lv_obj_set_style_bg_opa(ctx->cover_img, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    if (ctx->cover_placeholder != NULL) {
        lv_obj_add_flag(ctx->cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    mp_apply_visual(ctx);
}

static void mp_request_cover(music_ctx_t *ctx, const char *url)
{
    if (ctx == NULL || url == NULL || url[0] == '\0') return;

    bool same_url = (strncmp(ctx->cover_url, url, sizeof(ctx->cover_url)) == 0);
    int64_t now_ms = mp_now_ms();
    if (same_url && (ctx->cover_request_inflight || ctx->cover_dsc.data != NULL || now_ms < ctx->cover_retry_after_ms)) {
        return;
    }
    if (!same_url) {
        ctx->cover_fail_count = 0;
        ctx->cover_retry_after_ms = 0;
    }
    snprintf(ctx->cover_url, sizeof(ctx->cover_url), "%s", url);
    int tgt = MUSIC_COVER_TARGET;
    ctx->cover_request_inflight = true;
    if (ha_cover_fetcher_request(url, tgt, tgt, mp_cover_cb, ctx) != ESP_OK) {
        ctx->cover_request_inflight = false;
        mp_note_cover_failure(ctx);
    }
}

/* ---- Player list -------------------------------------------------------- */

static const char *mp_current_player(const music_ctx_t *ctx)
{
    if (ctx == NULL || ctx->config.player_count <= 0) return "";
    int idx = mp_clamp_int(ctx->current_player_index, 0, ctx->config.player_count - 1);
    return ctx->config.players[idx];
}

static void mp_derive_display_name(const char *entity_id, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0) return;
    const char *dot = strchr(entity_id, '.');
    const char *src = dot != NULL ? dot + 1 : entity_id;
    size_t i = 0;
    while (src[i] != '\0' && i + 1 < out_sz) {
        out[i] = (src[i] == '_') ? ' ' : src[i];
        i++;
    }
    out[i] = '\0';
    if (out[0] == '\0') {
        snprintf(out, out_sz, "Player");
    }
}

/* A Music Assistant player is a media_player whose state attributes carry
 * app_id == "music_assistant". Native WiiM/LinkPlay (or other) integrations
 * report a different/empty app_id and do NOT support MA playlist browsing. */
static bool mp_is_music_assistant_player(const char *entity_id)
{
    if (entity_id == NULL || entity_id[0] == '\0') {
        return false;
    }
    ha_state_t state = {0};
    if (!ha_model_get_state(entity_id, &state)) {
        return false;
    }
    bool is_ma = false;
    cJSON *attrs = cJSON_Parse(state.attributes_json);
    if (attrs != NULL) {
        const char *app_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(attrs, "app_id"));
        is_ma = (app_id != NULL && strcmp(app_id, "music_assistant") == 0);
        cJSON_Delete(attrs);
    }
    return is_ma;
}

static bool mp_entity_in_list(const char (*list)[APP_MAX_ENTITY_ID_LEN], int count, const char *id)
{
    if (id == NULL || id[0] == '\0') return false;
    for (int i = 0; i < count; i++) {
        if (strncmp(list[i], id, APP_MAX_ENTITY_ID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

/* Rebuild ctx->config.players from the configured players + the HA model, with
 * Music Assistant players first, and return the index of the preferred player.
 * Idempotent; safe to call on every state refresh. */
static int mp_build_player_list_and_pick(music_ctx_t *ctx)
{
    if (ctx == NULL) return 0;

    int written = 0;
    for (int i = 0; i < ctx->config.player_count && written < UI_MUSIC_MAX_PLAYERS; i++) {
        if (written != i) {
            memmove(ctx->config.players[written], ctx->config.players[i], APP_MAX_ENTITY_ID_LEN);
        }
        written++;
    }
    if (ctx->config.player_entity_id[0] != '\0' &&
        !mp_entity_in_list(ctx->config.players, written, ctx->config.player_entity_id) &&
        written < UI_MUSIC_MAX_PLAYERS) {
        snprintf(ctx->config.players[written], APP_MAX_ENTITY_ID_LEN, "%s", ctx->config.player_entity_id);
        written++;
    }

    ha_entity_info_t *infos = ui_calloc_prefer_psram(UI_MUSIC_MAX_PLAYERS, sizeof(*infos));
    if (infos != NULL) {
        size_t n = ha_model_list_entities("media_player", NULL, infos, UI_MUSIC_MAX_PLAYERS);
        /* Pass 1: Music Assistant players. */
        for (size_t i = 0; i < n && written < UI_MUSIC_MAX_PLAYERS; i++) {
            if (!mp_is_music_assistant_player(infos[i].id)) continue;
            if (mp_entity_in_list(ctx->config.players, written, infos[i].id)) continue;
            snprintf(ctx->config.players[written], APP_MAX_ENTITY_ID_LEN, "%s", infos[i].id);
            written++;
        }
        /* Pass 2: every other media_player (kept as fallback). */
        for (size_t i = 0; i < n && written < UI_MUSIC_MAX_PLAYERS; i++) {
            if (mp_is_music_assistant_player(infos[i].id)) continue;
            if (mp_entity_in_list(ctx->config.players, written, infos[i].id)) continue;
            snprintf(ctx->config.players[written], APP_MAX_ENTITY_ID_LEN, "%s", infos[i].id);
            written++;
        }
        heap_caps_free(infos);
    }
    ctx->config.player_count = written;

    /* Preferred: the configured player when it is an MA player, otherwise the
     * first MA player, otherwise the configured player, otherwise the first. */
    int best = -1;
    if (ctx->config.player_entity_id[0] != '\0' &&
        mp_is_music_assistant_player(ctx->config.player_entity_id)) {
        for (int i = 0; i < written; i++) {
            if (strncmp(ctx->config.players[i], ctx->config.player_entity_id, APP_MAX_ENTITY_ID_LEN) == 0) {
                best = i;
                break;
            }
        }
    }
    if (best < 0) {
        for (int i = 0; i < written; i++) {
            if (mp_is_music_assistant_player(ctx->config.players[i])) {
                best = i;
                break;
            }
        }
    }
    if (best < 0 && ctx->config.player_entity_id[0] != '\0') {
        for (int i = 0; i < written; i++) {
            if (strncmp(ctx->config.players[i], ctx->config.player_entity_id, APP_MAX_ENTITY_ID_LEN) == 0) {
                best = i;
                break;
            }
        }
    }
    if (best < 0) best = 0;
    return best;
}

static void mp_update_player_chip(music_ctx_t *ctx)
{
    if (ctx == NULL || ctx->player_chip_label == NULL) return;
    const char *entity_id = mp_current_player(ctx);
    char name[APP_MAX_NAME_LEN];
    mp_derive_display_name(entity_id, name, sizeof(name));

    /* Prefer the friendly name from the HA model when available.
     * Heap-allocated: UI_MUSIC_MAX_PLAYERS × sizeof(ha_entity_info_t) is
     * several KB and would overflow the small main-task stack this runs on. */
    ha_entity_info_t *infos = ui_calloc_prefer_psram(UI_MUSIC_MAX_PLAYERS, sizeof(*infos));
    if (infos != NULL) {
        size_t n = ha_model_list_entities("media_player", NULL, infos, UI_MUSIC_MAX_PLAYERS);
        for (size_t i = 0; i < n; i++) {
            if (strncmp(infos[i].id, entity_id, APP_MAX_ENTITY_ID_LEN) == 0 && infos[i].name[0] != '\0') {
                snprintf(name, sizeof(name), "%s", infos[i].name);
                break;
            }
        }
        heap_caps_free(infos);
    }

    char buf[APP_MAX_NAME_LEN + 8];
    snprintf(buf, sizeof(buf), "%s  " LV_SYMBOL_DOWN, name);
    lv_label_set_text(ctx->player_chip_label, buf);
}

static void mp_switch_to_player(music_ctx_t *ctx, int index)
{
    if (ctx == NULL || ctx->config.player_count <= 0) return;
    ctx->current_player_index = mp_clamp_int(index, 0, ctx->config.player_count - 1);

    /* Drop any in-flight/owned cover art from the previous player. */
    mp_release_cover(ctx);
    ctx->cover_url[0] = '\0';
    ctx->cover_dominant_rgb = 0;
    ctx->cover_fail_count = 0;
    ctx->cover_retry_after_ms = 0;
    ctx->duration_s = 0;
    ctx->position_s = 0;
    ctx->position_anchor_s = 0;
    ctx->now_title[0] = '\0';
    ctx->now_artist[0] = '\0';
    ctx->unavailable = true;
    ctx->is_playing = false;

    mp_update_player_chip(ctx);
    if (ctx->title_label) lv_label_set_text(ctx->title_label, "");
    if (ctx->artist_label) lv_label_set_text(ctx->artist_label, "");
    if (ctx->state_badge) lv_label_set_text(ctx->state_badge, "--");
    mp_cover_show_placeholder(ctx);
    mp_update_progress(ctx);
    mp_apply_visual(ctx);

    ha_state_t state = {0};
    if (ha_model_get_state(mp_current_player(ctx), &state)) {
        ui_music_page_apply_state(&(ui_music_page_instance_t){ .ctx = ctx, .obj = ctx->root }, &state);
    }
}

/* ---- Music Assistant library (playlists / search) ------------------------ */

static lv_obj_t *mp_create_icon_button(lv_obj_t *parent, const char *symbol, const lv_font_t *font,
    music_ctx_t *ctx, lv_event_cb_t cb, lv_obj_t **out_label, lv_coord_t size);
static void mp_lib_item_clicked(lv_event_t *event);
static void mp_lib_hide(music_ctx_t *ctx);
static void mp_lib_render_async(void *arg);

static void mp_lib_free_result(music_ctx_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->lib_result != NULL) {
        heap_caps_free(ctx->lib_result);
        ctx->lib_result = NULL;
    }
}

static void mp_lib_set_flex_center(music_ctx_t *ctx, bool center)
{
    if (ctx == NULL || ctx->lib_list == NULL) return;
    lv_obj_set_flex_align(ctx->lib_list,
        center ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
        center ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START);
}

static void mp_lib_show_message(music_ctx_t *ctx, const char *text)
{
    if (ctx == NULL || ctx->lib_list == NULL) return;
    mp_lib_set_flex_center(ctx, true);
    lv_obj_t *lbl = lv_label_create(ctx->lib_list);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, MUSIC_LIB_LIST_W - 32);
    lv_obj_set_style_text_font(lbl, APP_FONT_TEXT_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(lbl, text != NULL ? text : "");
}

static void mp_lib_render_result(music_ctx_t *ctx, music_lib_result_t *res)
{
    if (ctx == NULL || ctx->lib_list == NULL) {
        if (res != NULL) {
            heap_caps_free(res);
        }
        return;
    }

    mp_lib_free_result(ctx);
    if (res != NULL) {
        ctx->lib_result = res;
    }
    lv_obj_clean(ctx->lib_list);

    if (res == NULL || !res->ok || res->count <= 0) {
        mp_lib_show_message(ctx, res == NULL ? "Błąd połączenia" : (res->count == 0 ? "Brak wyników" : "Błąd"));
        return;
    }

    mp_lib_set_flex_center(ctx, false);

    lv_color_t surface = lv_color_mix(lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE),
        lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), 224);
    lv_color_t border = lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER);

    for (int i = 0; i < res->count; i++) {
        music_lib_item_t *item = &res->items[i];
        lv_obj_t *btn = lv_btn_create(ctx->lib_list);
        lv_obj_set_size(btn, MUSIC_LIB_LIST_W - 12, 46);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_left(btn, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_right(btn, 14, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, surface, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, mp_pressed_variant(surface), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, border, LV_PART_MAIN);
        lv_obj_set_style_border_opa(btn, LV_OPA_70, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, mp_lib_item_clicked, LV_EVENT_CLICKED, ctx);

        char text[MUSIC_LIB_TITLE_MAX + 8];
        snprintf(text, sizeof(text), "%s%s", item->title, item->can_play ? "  " LV_SYMBOL_RIGHT : "");
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, MUSIC_LIB_LIST_W - 72);
        lv_obj_set_style_text_font(label, APP_FONT_TEXT_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

static void mp_lib_render_async(void *arg)
{
    music_lib_result_t *res = (music_lib_result_t *)arg;
    if (res == NULL) return;
    music_ctx_t *ctx = res->ctx;
    if (ctx == NULL || s_live_music_ctx != ctx || res->gen != ctx->lib_req_gen) {
        heap_caps_free(res);
        return;
    }
    mp_lib_render_result(ctx, res);
}

static void mp_lib_response_cb(bool success, cJSON *result_payload, void *user)
{
    music_ctx_t *ctx = (music_ctx_t *)user;
    if (ctx == NULL) return;

    music_lib_result_t *res = ui_calloc_prefer_psram(1,
        sizeof(music_lib_result_t) + MUSIC_LIB_MAX_ITEMS * sizeof(music_lib_item_t));
    if (res == NULL) return;
    res->ctx = ctx;
    res->gen = ctx->lib_req_gen;
    res->ok = success && result_payload != NULL;
    res->count = 0;

    if (res->ok) {
        /* result_payload is the "result" object:
         * { response: { <entity_id>: { children: [...] or result: [...] } } } */
        cJSON *response = cJSON_GetObjectItemCaseSensitive(result_payload, "response");
        cJSON *list = NULL;
        if (cJSON_IsObject(response)) {
            for (cJSON *per_entity = response->child; per_entity != NULL; per_entity = per_entity->next) {
                if (!cJSON_IsObject(per_entity)) continue;
                list = cJSON_GetObjectItemCaseSensitive(per_entity, "children");
                if (list == NULL) {
                    list = cJSON_GetObjectItemCaseSensitive(per_entity, "result");
                }
                if (list != NULL) break;
            }
        }

        if (cJSON_IsArray(list)) {
            cJSON *child = NULL;
            cJSON_ArrayForEach(child, list) {
                if (res->count >= MUSIC_LIB_MAX_ITEMS || !cJSON_IsObject(child)) continue;
                music_lib_item_t *item = &res->items[res->count];
                memset(item, 0, sizeof(*item));
                const char *title = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(child, "title"));
                const char *cls = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(child, "media_class"));
                const char *ctype = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(child, "media_content_type"));
                const char *cid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(child, "media_content_id"));
                cJSON *can_play = cJSON_GetObjectItemCaseSensitive(child, "can_play");
                if (title != NULL) snprintf(item->title, sizeof(item->title), "%s", title);
                if (cls != NULL) snprintf(item->media_class, sizeof(item->media_class), "%s", cls);
                if (ctype != NULL) snprintf(item->media_content_type, sizeof(item->media_content_type), "%s", ctype);
                if (cid != NULL) snprintf(item->media_content_id, sizeof(item->media_content_id), "%s", cid);
                item->can_play = cJSON_IsTrue(can_play);
                res->count++;
            }
        }
    }

    lv_async_call(mp_lib_render_async, res);
}

static void mp_lib_item_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    lv_obj_t *btn = lv_event_get_target_obj(event);
    if (ctx == NULL || btn == NULL || ctx->lib_result == NULL) return;
    int idx = (int)lv_obj_get_index(btn);
    if (idx < 0 || idx >= ctx->lib_result->count) return;
    music_lib_item_t *item = &ctx->lib_result->items[idx];

    const char *entity = mp_current_player(ctx);
    if (entity == NULL || ctx->unavailable) return;

    if (item->can_play) {
        (void)ui_bindings_media_play_item(entity, item->media_content_type, item->media_content_id);
        mp_lib_hide(ctx);
    }
}

static void mp_lib_set_title(music_ctx_t *ctx, const char *title)
{
    if (ctx == NULL) return;
    lv_label_set_text(ctx->lib_title_label, title ? title : "");
}

static void mp_lib_load(music_ctx_t *ctx, int mode)
{
    if (ctx == NULL) return;
    const char *entity = mp_current_player(ctx);
    if (entity == NULL || ctx->unavailable) {
        lv_obj_clean(ctx->lib_list);
        mp_lib_show_message(ctx, "Brak aktywnego odtwarzacza");
        return;
    }

    ctx->lib_mode = mode;
    ctx->lib_req_gen++;
    ha_client_cancel_pending_responses_for_user(ctx);
    mp_lib_free_result(ctx);
    lv_obj_clean(ctx->lib_list);
    mp_lib_show_message(ctx, "Wczytywanie...");

    if (mode == MP_LIB_MODE_PLAYLISTS) {
        mp_lib_set_title(ctx, "Playlisty");
        (void)ui_bindings_media_browse(entity, "music_assistant", "playlists", mp_lib_response_cb, ctx);
    } else if (mode == MP_LIB_MODE_SEARCH) {
        mp_lib_set_title(ctx, "Szukaj utworów");
    }
}

static void mp_lib_do_search(music_ctx_t *ctx, const char *query)
{
    if (ctx == NULL || query == NULL || query[0] == '\0') return;
    const char *entity = mp_current_player(ctx);
    if (entity == NULL || ctx->unavailable) return;

    ctx->lib_req_gen++;
    ha_client_cancel_pending_responses_for_user(ctx);
    mp_lib_free_result(ctx);
    lv_obj_clean(ctx->lib_list);
    mp_lib_set_title(ctx, "Wyniki wyszukiwania");
    mp_lib_show_message(ctx, "Wczytywanie...");

    (void)ui_bindings_media_search(entity, query, mp_lib_response_cb, ctx);
}

static void mp_lib_hide(music_ctx_t *ctx)
{
    if (ctx == NULL) return;
    ctx->lib_mode = MP_LIB_MODE_NONE;
    if (ctx->keyboard != NULL) {
        lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (ctx->lib_overlay != NULL) {
        lv_obj_add_flag(ctx->lib_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    mp_apply_visual(ctx);
}

static void mp_lib_show(music_ctx_t *ctx, int mode)
{
    if (ctx == NULL || ctx->lib_overlay == NULL) return;

    bool search = (mode == MP_LIB_MODE_SEARCH);
    if (ctx->search_row != NULL) {
        if (search) {
            lv_obj_clear_flag(ctx->search_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ctx->search_row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ctx->keyboard != NULL) {
        if (search) {
            lv_obj_clear_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(ctx->keyboard);
        } else {
            lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ctx->lib_list != NULL) {
        if (search) {
            lv_obj_set_pos(ctx->lib_list, MUSIC_LIB_LIST_X, MUSIC_LIB_SEARCH_LIST_Y);
            lv_obj_set_size(ctx->lib_list, MUSIC_LIB_LIST_W, MUSIC_LIB_SEARCH_LIST_H);
        } else {
            lv_obj_set_pos(ctx->lib_list, MUSIC_LIB_LIST_X, MUSIC_LIB_LIST_Y);
            lv_obj_set_size(ctx->lib_list, MUSIC_LIB_LIST_W, MUSIC_LIB_LIST_H);
        }
    }

    lv_obj_clear_flag(ctx->lib_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ctx->lib_overlay);
    mp_lib_load(ctx, mode);
}

static void mp_lib_back_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL) return;
    mp_lib_hide(ctx);
}

static void mp_tab_playlists_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->unavailable) return;
    mp_lib_show(ctx, MP_LIB_MODE_PLAYLISTS);
}

static void mp_tab_search_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->unavailable) return;
    mp_lib_show(ctx, MP_LIB_MODE_SEARCH);
}

static void mp_lib_search_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->search_textarea == NULL) return;
    const char *q = lv_textarea_get_text(ctx->search_textarea);
    if (q == NULL || q[0] == '\0') return;
    mp_lib_do_search(ctx, q);
}

static void mp_lib_textarea_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->keyboard == NULL || ctx->lib_overlay == NULL) return;
    if (ctx->lib_mode != MP_LIB_MODE_SEARCH) return;
    lv_obj_clear_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ctx->keyboard);
}

static void mp_lib_create(music_ctx_t *ctx)
{
    if (ctx == NULL || ctx->root == NULL) return;

    lv_color_t surface = lv_color_mix(lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE),
        lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), 224);
    lv_color_t border = lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER);
    lv_color_t text_primary = lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY);

    lv_obj_t *overlay = lv_obj_create(ctx->root);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, APP_CONTENT_BOX_WIDTH, APP_CONTENT_BOX_HEIGHT);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    ctx->lib_overlay = overlay;

    /* Back button */
    lv_obj_t *back_label = NULL;
    ctx->lib_back_btn = mp_create_icon_button(overlay, LV_SYMBOL_LEFT, APP_FONT_DISPLAY_24, ctx,
        mp_lib_back_clicked, &back_label, MUSIC_LIB_BACK_SIZE);
    lv_obj_set_pos(ctx->lib_back_btn, MUSIC_LIB_PAD, (MUSIC_LIB_HEADER_H - MUSIC_LIB_BACK_SIZE) / 2);
    lv_obj_set_style_bg_color(ctx->lib_back_btn, surface, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ctx->lib_back_btn, mp_pressed_variant(surface), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(ctx->lib_back_btn, border, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, text_primary, LV_PART_MAIN);

    /* Title */
    ctx->lib_title_label = lv_label_create(overlay);
    lv_obj_set_pos(ctx->lib_title_label, MUSIC_LIB_TITLE_X, (MUSIC_LIB_HEADER_H - 22) / 2);
    lv_obj_set_size(ctx->lib_title_label, MUSIC_LIB_TITLE_W, 22);
    lv_obj_set_style_text_font(ctx->lib_title_label, APP_FONT_TEXT_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->lib_title_label, text_primary, LV_PART_MAIN);
    lv_label_set_long_mode(ctx->lib_title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(ctx->lib_title_label, "Playlisty");

    /* Results list */
    ctx->lib_list = lv_obj_create(overlay);
    lv_obj_remove_style_all(ctx->lib_list);
    lv_obj_set_size(ctx->lib_list, MUSIC_LIB_LIST_W, MUSIC_LIB_LIST_H);
    lv_obj_set_pos(ctx->lib_list, MUSIC_LIB_LIST_X, MUSIC_LIB_LIST_Y);
    lv_obj_set_flex_flow(ctx->lib_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctx->lib_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(ctx->lib_list, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ctx->lib_list, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->lib_list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->lib_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(ctx->lib_list, LV_SCROLLBAR_MODE_OFF);

    /* Search row (textarea + button) */
    ctx->search_row = lv_obj_create(overlay);
    lv_obj_remove_style_all(ctx->search_row);
    lv_obj_set_size(ctx->search_row, APP_CONTENT_BOX_WIDTH - 2 * MUSIC_LIB_PAD, MUSIC_LIB_SEARCH_H);
    lv_obj_set_pos(ctx->search_row, MUSIC_LIB_SEARCH_X, MUSIC_LIB_SEARCH_Y);
    lv_obj_clear_flag(ctx->search_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ctx->search_row, 0, LV_PART_MAIN);
    lv_obj_add_flag(ctx->search_row, LV_OBJ_FLAG_HIDDEN);

    ctx->search_textarea = lv_textarea_create(ctx->search_row);
    lv_obj_set_size(ctx->search_textarea, MUSIC_LIB_SEARCH_W, MUSIC_LIB_SEARCH_H);
    lv_obj_set_pos(ctx->search_textarea, 0, 0);
    lv_obj_set_style_radius(ctx->search_textarea, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_left(ctx->search_textarea, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(ctx->search_textarea, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->search_textarea, surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->search_textarea, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->search_textarea, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->search_textarea, border, LV_PART_MAIN);
    lv_obj_set_style_text_font(ctx->search_textarea, APP_FONT_TEXT_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->search_textarea, text_primary, LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->search_textarea, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_textarea_set_one_line(ctx->search_textarea, true);
    lv_textarea_set_max_length(ctx->search_textarea, 80);
    lv_textarea_set_placeholder_text(ctx->search_textarea, "Tytuł lub wykonawca...");
    lv_obj_add_event_cb(ctx->search_textarea, mp_lib_search_clicked, LV_EVENT_READY, ctx);
    lv_obj_add_event_cb(ctx->search_textarea, mp_lib_textarea_clicked, LV_EVENT_CLICKED, ctx);

    ctx->search_btn = lv_btn_create(ctx->search_row);
    lv_obj_set_size(ctx->search_btn, MUSIC_LIB_SEARCH_BTN_W, MUSIC_LIB_SEARCH_H);
    lv_obj_set_pos(ctx->search_btn, MUSIC_LIB_SEARCH_W + 8, 0);
    lv_obj_clear_flag(ctx->search_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ctx->search_btn, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->search_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ctx->search_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ctx->search_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->search_btn, surface, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ctx->search_btn, mp_pressed_variant(surface), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ctx->search_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->search_btn, border, LV_PART_MAIN);
    lv_obj_add_event_cb(ctx->search_btn, mp_lib_search_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_t *search_btn_label = lv_label_create(ctx->search_btn);
    lv_label_set_text(search_btn_label, "Szukaj");
    lv_obj_set_style_text_font(search_btn_label, APP_FONT_TEXT_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(search_btn_label, text_primary, LV_PART_MAIN);
    lv_obj_center(search_btn_label);

    /* Keyboard. lv_keyboard_create() aligns itself to BOTTOM_MID by default, so
     * reset the alignment to TOP_LEFT before positioning, otherwise the Y
     * offset is applied from the bottom and the keyboard ends up off-screen. */
    ctx->keyboard = lv_keyboard_create(overlay);
    lv_obj_set_size(ctx->keyboard, APP_CONTENT_BOX_WIDTH, MUSIC_LIB_KEYBOARD_H);
    lv_obj_align(ctx->keyboard, LV_ALIGN_TOP_LEFT, 0, MUSIC_LIB_KEYBOARD_Y);
    lv_obj_add_flag(ctx->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(ctx->keyboard, ctx->search_textarea);
}

/* ---- Events ------------------------------------------------------------- */

static void mp_chip_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->config.player_count <= 1) return;
    mp_switch_to_player(ctx, ctx->current_player_index + 1);
}

static void mp_prev_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->unavailable) return;
    (void)ui_bindings_media_player_action(mp_current_player(ctx), UI_BINDINGS_MEDIA_ACTION_PREVIOUS);
}

static void mp_play_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->unavailable) return;
    (void)ui_bindings_media_player_action(mp_current_player(ctx), UI_BINDINGS_MEDIA_ACTION_PLAY_PAUSE);
}

static void mp_next_clicked(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->unavailable) return;
    (void)ui_bindings_media_player_action(mp_current_player(ctx), UI_BINDINGS_MEDIA_ACTION_NEXT);
}

static void mp_volume_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->vol_slider == NULL || ctx->unavailable) return;

    if (code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
        ctx->volume_dragging = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ctx->volume_dragging = false;
        int pct = (int)lv_slider_get_value(ctx->vol_slider);
        pct = mp_clamp_int(pct, 0, 100);
        (void)ui_bindings_set_slider_value(mp_current_player(ctx), pct);
    }
}

static void mp_tick_cb(lv_timer_t *timer)
{
    system_log_note_lvgl_cb("mp_tick_cb");
    music_ctx_t *ctx = (music_ctx_t *)lv_timer_get_user_data(timer);
    if (ctx == NULL) return;
    if (ctx->is_playing && !ctx->unavailable) {
        mp_update_progress(ctx);
    }
}

static void mp_delete_cb(lv_event_t *event)
{
    music_ctx_t *ctx = (music_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL) return;
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        if (s_live_music_ctx == ctx) {
            s_live_music_ctx = NULL;
        }
        ha_client_cancel_pending_responses_for_user(ctx);
        mp_lib_free_result(ctx);
        ha_cover_fetcher_cancel(ctx);
        if (ctx->tick_timer != NULL) {
            lv_timer_del(ctx->tick_timer);
            ctx->tick_timer = NULL;
        }
        mp_release_cover(ctx);
        free(ctx);
    }
}

/* ---- State application -------------------------------------------------- */

static void mp_apply_state_internal(music_ctx_t *ctx, const ha_state_t *state)
{
    if (ctx == NULL || state == NULL) return;

    bool unavailable = mp_state_unavailable(state->state);
    bool playing = (strcmp(state->state, "playing") == 0);

    char title[MUSIC_MAX_TITLE_LEN] = "";
    char artist[MUSIC_MAX_TITLE_LEN] = "";
    char pic[MUSIC_MAX_COVER_URL_LEN] = "";
    int duration_s = 0;
    int position_s = 0;
    time_t position_anchor_s = 0;
    bool have_position = false;
    int volume_pct = ctx->volume_pct;
    bool volume_muted = ctx->volume_muted;

    cJSON *attrs = cJSON_Parse(state->attributes_json);
    if (attrs != NULL) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(attrs, "media_title");
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            snprintf(title, sizeof(title), "%s", item->valuestring);
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "media_artist");
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            snprintf(artist, sizeof(artist), "%s", item->valuestring);
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "entity_picture");
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            snprintf(pic, sizeof(pic), "%s", item->valuestring);
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "media_duration");
        if (cJSON_IsNumber(item)) {
            duration_s = (int)(item->valuedouble + 0.5);
            if (duration_s < 0) duration_s = 0;
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "media_position");
        if (cJSON_IsNumber(item)) {
            position_s = (int)(item->valuedouble + 0.5);
            if (position_s < 0) position_s = 0;
            have_position = true;
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "media_position_updated_at");
        if (cJSON_IsString(item) && item->valuestring != NULL) {
            time_t anchor = 0;
            if (mp_parse_iso8601_utc(item->valuestring, &anchor)) {
                position_anchor_s = anchor;
            }
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "volume_level");
        if (cJSON_IsNumber(item)) {
            double v = item->valuedouble;
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            volume_pct = (int)(v * 100.0 + 0.5);
        }
        item = cJSON_GetObjectItemCaseSensitive(attrs, "is_volume_muted");
        if (cJSON_IsBool(item)) {
            volume_muted = cJSON_IsTrue(item);
        }
        cJSON_Delete(attrs);
    }

    if (title[0] == '\0') {
        /* Fall back to the player's friendly name so the view never looks empty. */
        char name[APP_MAX_NAME_LEN];
        mp_derive_display_name(state->entity_id, name, sizeof(name));
        snprintf(title, sizeof(title), "%s", name);
    }

    ctx->unavailable = unavailable;
    ctx->is_playing = playing;
    ctx->duration_s = duration_s;
    ctx->volume_pct = mp_clamp_int(volume_pct, 0, 100);
    ctx->volume_muted = volume_muted;

    if (have_position) {
        ctx->position_s = position_s;
        ctx->position_anchor_s = playing ? ((position_anchor_s > 0) ? position_anchor_s : time(NULL)) : 0;
    } else {
        ctx->position_s = 0;
        ctx->position_anchor_s = 0;
    }

    if (ctx->title_label != NULL) {
        lv_label_set_text(ctx->title_label, title);
    }
    if (ctx->artist_label != NULL) {
        lv_label_set_text(ctx->artist_label, artist[0] != '\0' ? artist : "");
    }
    if (ctx->state_badge != NULL) {
        if (unavailable) {
            lv_label_set_text(ctx->state_badge, "OFFLINE");
        } else if (playing) {
            lv_label_set_text(ctx->state_badge, "PLAYING");
        } else {
            lv_label_set_text(ctx->state_badge, "PAUSED");
        }
    }

    if (strncmp(ctx->cover_url, pic, sizeof(ctx->cover_url)) != 0 && pic[0] != '\0') {
        mp_request_cover(ctx, pic);
    } else if (pic[0] == '\0' && ctx->cover_dsc.data != NULL) {
        mp_release_cover(ctx);
        mp_cover_show_placeholder(ctx);
    }

    mp_update_volume_visual(ctx);
    mp_update_progress(ctx);
    mp_apply_visual(ctx);
}

bool ui_music_page_apply_state(ui_music_page_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->ctx == NULL || state == NULL) {
        return false;
    }
    music_ctx_t *ctx = (music_ctx_t *)instance->ctx;
    const char *current = mp_current_player(ctx);
    if (current[0] == '\0' || strncmp(state->entity_id, current, APP_MAX_ENTITY_ID_LEN) != 0) {
        return false;
    }
    mp_apply_state_internal(ctx, state);
    return true;
}

bool ui_music_page_apply_state_detect_play_start(ui_music_page_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->ctx == NULL || state == NULL) {
        return false;
    }
    music_ctx_t *ctx = (music_ctx_t *)instance->ctx;
    const char *current = mp_current_player(ctx);
    if (current[0] == '\0' || strncmp(state->entity_id, current, APP_MAX_ENTITY_ID_LEN) != 0) {
        return false;
    }

    bool was_playing = ctx->is_playing;
    mp_apply_state_internal(ctx, state);
    return ctx->is_playing && !was_playing;
}

void ui_music_page_apply_all_states(ui_music_page_instance_t *instance)
{
    if (instance == NULL || instance->ctx == NULL) {
        return;
    }
    music_ctx_t *ctx = (music_ctx_t *)instance->ctx;
    const char *current = mp_current_player(ctx);
    if (current[0] == '\0') {
        return;
    }

    /* Once MA player states arrive, auto-switch away from a native (non-MA)
     * player so playlists/search work without the user typing the right ID. */
    if (!mp_is_music_assistant_player(current)) {
        int best = mp_build_player_list_and_pick(ctx);
        if (best >= 0 && best < ctx->config.player_count &&
            strncmp(ctx->config.players[best], current, APP_MAX_ENTITY_ID_LEN) != 0 &&
            mp_is_music_assistant_player(ctx->config.players[best])) {
            ESP_LOGI(TAG, "auto-switching player '%s' -> Music Assistant player '%s'",
                     current, ctx->config.players[best]);
            mp_switch_to_player(ctx, best);
            return;
        }
    }

    ha_state_t state = {0};
    if (ha_model_get_state(current, &state)) {
        mp_apply_state_internal(ctx, &state);
    } else {
        ctx->unavailable = true;
        ctx->is_playing = false;
        if (ctx->state_badge) lv_label_set_text(ctx->state_badge, "OFFLINE");
        mp_apply_visual(ctx);
    }
}

/* ---- Creation ----------------------------------------------------------- */

static lv_obj_t *mp_create_icon_button(lv_obj_t *parent, const char *symbol, const lv_font_t *font, music_ctx_t *ctx,
    lv_event_cb_t cb, lv_obj_t **out_label, lv_coord_t size)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ctx);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    if (font != NULL) {
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    }
    lv_obj_center(label);
    if (out_label != NULL) {
        *out_label = label;
    }
    return btn;
}

esp_err_t ui_music_page_create(
    const ui_music_page_config_t *config, lv_obj_t *parent, ui_music_page_instance_t *out_instance)
{
    if (config == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    music_ctx_t *ctx = ui_calloc_prefer_psram(1, sizeof(music_ctx_t));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->config = *config;
    ctx->current_player_index = 0;
    ctx->unavailable = true;
    ctx->volume_pct = 50;

    /* Build the player list from config + HA model and pick the best player.
     * Music Assistant players are preferred so the page keeps working even if
     * the configured entity is a native (non-MA) integration. At boot the HA
     * model may still be empty, so the configured player is always kept and
     * the selection is re-evaluated when states arrive. */
    ctx->current_player_index = mp_build_player_list_and_pick(ctx);

    /* Root */
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, APP_CONTENT_BOX_WIDTH, APP_CONTENT_BOX_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* Transparent on purpose: the page container painted by ui_page_style owns
     * the background, so a page colour/gradient/wallpaper shows through. */
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    ctx->root = root;

    /* Hero panel */
    lv_obj_t *hero = lv_obj_create(root);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, MUSIC_HERO_W, MUSIC_HERO_H);
    lv_obj_set_pos(hero, MUSIC_HERO_X, MUSIC_HERO_Y);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(hero, MUSIC_HERO_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hero, lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hero, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(hero, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(hero, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hero, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(hero, true, LV_PART_MAIN);
    ctx->hero = hero;

    /* Player selector chip */
    lv_obj_t *chip = lv_btn_create(hero);
    lv_obj_set_size(chip, MUSIC_CHIP_W, MUSIC_CHIP_H);
    lv_obj_set_pos(chip, MUSIC_CHIP_X, MUSIC_CHIP_Y);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(chip, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(chip, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(chip, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_event_cb(chip, mp_chip_clicked, LV_EVENT_CLICKED, ctx);
    ctx->player_chip = chip;

    ctx->player_chip_label = lv_label_create(chip);
    lv_obj_set_style_text_font(ctx->player_chip_label, MUSIC_CHIP_FONT, LV_PART_MAIN);
    lv_label_set_long_mode(ctx->player_chip_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ctx->player_chip_label, MUSIC_CHIP_W - 20);
    lv_obj_set_style_text_align(ctx->player_chip_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(ctx->player_chip_label);

    /* State badge */
    ctx->state_badge = lv_label_create(hero);
    lv_obj_set_size(ctx->state_badge, MUSIC_BADGE_W, MUSIC_BADGE_H);
    lv_obj_set_pos(ctx->state_badge, MUSIC_BADGE_X, MUSIC_BADGE_Y);
    lv_obj_set_style_radius(ctx->state_badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_top(ctx->state_badge, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(ctx->state_badge, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_left(ctx->state_badge, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(ctx->state_badge, 8, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ctx->state_badge, true, LV_PART_MAIN);
    lv_obj_set_style_text_font(ctx->state_badge, MUSIC_BADGE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->state_badge, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(ctx->state_badge, "--");

    /* Library tab buttons */
    lv_color_t control_surface = lv_color_mix(lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE),
        lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), 224);
    lv_color_t control_border = lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BORDER);

    ctx->btn_playlists = lv_btn_create(hero);
    lv_obj_set_size(ctx->btn_playlists, MUSIC_BTN_LIST_W, MUSIC_BTN_TAB_H);
    lv_obj_set_pos(ctx->btn_playlists, MUSIC_BTN_LIST_X, MUSIC_BTN_TAB_Y);
    lv_obj_clear_flag(ctx->btn_playlists, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ctx->btn_playlists, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->btn_playlists, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ctx->btn_playlists, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ctx->btn_playlists, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->btn_playlists, control_surface, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ctx->btn_playlists, mp_pressed_variant(control_surface),
        LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ctx->btn_playlists, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->btn_playlists, control_border, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ctx->btn_playlists, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_event_cb(ctx->btn_playlists, mp_tab_playlists_clicked, LV_EVENT_CLICKED, ctx);
    ctx->btn_playlists_label = lv_label_create(ctx->btn_playlists);
    lv_label_set_text(ctx->btn_playlists_label, "Listy");
    lv_obj_set_style_text_font(ctx->btn_playlists_label, MUSIC_BTN_TAB_FONT, LV_PART_MAIN);
    lv_obj_center(ctx->btn_playlists_label);

    ctx->btn_search = lv_btn_create(hero);
    lv_obj_set_size(ctx->btn_search, MUSIC_BTN_SEARCH_W, MUSIC_BTN_TAB_H);
    lv_obj_set_pos(ctx->btn_search, MUSIC_BTN_SEARCH_X, MUSIC_BTN_TAB_Y);
    lv_obj_clear_flag(ctx->btn_search, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ctx->btn_search, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->btn_search, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ctx->btn_search, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ctx->btn_search, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->btn_search, control_surface, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ctx->btn_search, mp_pressed_variant(control_surface),
        LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ctx->btn_search, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->btn_search, control_border, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ctx->btn_search, LV_OPA_80, LV_PART_MAIN);
    lv_obj_add_event_cb(ctx->btn_search, mp_tab_search_clicked, LV_EVENT_CLICKED, ctx);
    ctx->btn_search_label = lv_label_create(ctx->btn_search);
    lv_label_set_text(ctx->btn_search_label, "Szukaj");
    lv_obj_set_style_text_font(ctx->btn_search_label, MUSIC_BTN_TAB_FONT, LV_PART_MAIN);
    lv_obj_center(ctx->btn_search_label);

    /* Cover image + placeholder */
    ctx->cover_img = lv_image_create(hero);
    lv_obj_set_size(ctx->cover_img, MUSIC_COVER_SIZE, MUSIC_COVER_SIZE);
    lv_obj_set_pos(ctx->cover_img, MUSIC_COVER_X, MUSIC_COVER_Y);
    lv_obj_set_style_radius(ctx->cover_img, 18, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ctx->cover_img, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->cover_img, lv_color_mix(lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_IDLE),
        lv_color_hex(APP_UI_COLOR_CARD_BG_OFF), 184), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->cover_img, LV_OPA_COVER, LV_PART_MAIN);

    ctx->cover_placeholder = lv_label_create(hero);
    lv_label_set_text(ctx->cover_placeholder, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(ctx->cover_placeholder, APP_FONT_DISPLAY_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->cover_placeholder, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_set_size(ctx->cover_placeholder, MUSIC_COVER_SIZE, MUSIC_COVER_SIZE);
    lv_obj_set_pos(ctx->cover_placeholder, MUSIC_COVER_X, MUSIC_COVER_Y);
    lv_obj_set_style_text_align(ctx->cover_placeholder, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(ctx->cover_placeholder, MUSIC_COVER_SIZE / 2 - 24, LV_PART_MAIN);

    /* Title + artist */
    ctx->title_label = lv_label_create(hero);
    lv_obj_set_pos(ctx->title_label, MUSIC_INFO_X, MUSIC_TITLE_Y);
    lv_obj_set_size(ctx->title_label, MUSIC_INFO_W, MUSIC_TITLE_H);
    lv_obj_set_style_text_font(ctx->title_label, MUSIC_TITLE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->title_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(ctx->title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(ctx->title_label, "");

    ctx->artist_label = lv_label_create(hero);
    lv_obj_set_pos(ctx->artist_label, MUSIC_INFO_X, MUSIC_ARTIST_Y);
    lv_obj_set_size(ctx->artist_label, MUSIC_INFO_W, MUSIC_ARTIST_H);
    lv_obj_set_style_text_font(ctx->artist_label, MUSIC_ARTIST_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->artist_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(ctx->artist_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(ctx->artist_label, "");

    /* Progress bar + time labels */
    ctx->progress_bar = lv_bar_create(hero);
    lv_obj_set_pos(ctx->progress_bar, MUSIC_PROG_X, MUSIC_PROG_Y);
    lv_obj_set_size(ctx->progress_bar, MUSIC_PROG_W, MUSIC_PROG_H);
    lv_bar_set_range(ctx->progress_bar, 0, 100);
    lv_bar_set_value(ctx->progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(ctx->progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(ctx->progress_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->progress_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(ctx->progress_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ctx->progress_bar, true, LV_PART_MAIN);

    ctx->pos_label = lv_label_create(hero);
    lv_obj_set_pos(ctx->pos_label, MUSIC_POS_X, MUSIC_TIME_Y);
    lv_obj_set_size(ctx->pos_label, MUSIC_POS_W, 20);
    lv_obj_set_style_text_font(ctx->pos_label, MUSIC_TIME_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->pos_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_text(ctx->pos_label, "0:00");

    ctx->dur_label = lv_label_create(hero);
    lv_obj_set_pos(ctx->dur_label, MUSIC_DUR_X, MUSIC_TIME_Y);
    lv_obj_set_size(ctx->dur_label, MUSIC_DUR_W, 20);
    lv_obj_set_style_text_font(ctx->dur_label, MUSIC_TIME_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->dur_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_text(ctx->dur_label, "--:--");

    /* Transport controls */
    lv_coord_t small_center_y = MUSIC_TRANSPORT_Y + (MUSIC_BTN_PLAY_SIZE - MUSIC_BTN_SMALL) / 2;
    ctx->btn_prev = mp_create_icon_button(hero, LV_SYMBOL_PREV, MUSIC_NAV_FONT, ctx, mp_prev_clicked,
        &ctx->btn_prev_label, MUSIC_BTN_SMALL);
    lv_obj_set_pos(ctx->btn_prev, MUSIC_BTN_PREV_X, small_center_y);

    ctx->btn_play = mp_create_icon_button(hero, LV_SYMBOL_PLAY, MUSIC_PLAY_FONT, ctx, mp_play_clicked,
        &ctx->btn_play_label, MUSIC_BTN_PLAY_SIZE);
    lv_obj_set_pos(ctx->btn_play, MUSIC_BTN_PLAY_X, MUSIC_TRANSPORT_Y);

    ctx->btn_next = mp_create_icon_button(hero, LV_SYMBOL_NEXT, MUSIC_NAV_FONT, ctx, mp_next_clicked,
        &ctx->btn_next_label, MUSIC_BTN_SMALL);
    lv_obj_set_pos(ctx->btn_next, MUSIC_BTN_NEXT_X, small_center_y);

    /* Volume */
    ctx->vol_icon = lv_label_create(hero);
    lv_obj_set_pos(ctx->vol_icon, MUSIC_VOL_ICON_X, MUSIC_VOL_ICON_Y);
    lv_obj_set_style_text_font(ctx->vol_icon, MUSIC_VOL_ICON_FONT, LV_PART_MAIN);
    lv_label_set_text(ctx->vol_icon, LV_SYMBOL_VOLUME_MID);

    ctx->vol_slider = lv_slider_create(hero);
    lv_obj_set_pos(ctx->vol_slider, MUSIC_VOL_SLIDER_X, MUSIC_VOL_SLIDER_Y);
    lv_obj_set_size(ctx->vol_slider, MUSIC_VOL_SLIDER_W, MUSIC_VOL_SLIDER_H);
    lv_slider_set_range(ctx->vol_slider, 0, 100);
    lv_slider_set_value(ctx->vol_slider, ctx->volume_pct, LV_ANIM_OFF);
    lv_obj_set_style_radius(ctx->vol_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->vol_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ctx->vol_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_width(ctx->vol_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->vol_slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(ctx->vol_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ctx->vol_slider, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ctx->vol_slider, true, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ctx->vol_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(ctx->vol_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_transform_width(ctx->vol_slider, -4, LV_PART_KNOB);
    lv_obj_set_style_transform_height(ctx->vol_slider, -4, LV_PART_KNOB);
    lv_obj_add_event_cb(ctx->vol_slider, mp_volume_event, LV_EVENT_PRESSING, ctx);
    lv_obj_add_event_cb(ctx->vol_slider, mp_volume_event, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(ctx->vol_slider, mp_volume_event, LV_EVENT_RELEASED, ctx);
    lv_obj_add_event_cb(ctx->vol_slider, mp_volume_event, LV_EVENT_PRESS_LOST, ctx);
    ui_slider_touch_enable(ctx->vol_slider);

    s_live_music_ctx = ctx;
    mp_lib_create(ctx);

    lv_obj_add_event_cb(root, mp_delete_cb, LV_EVENT_DELETE, ctx);

    mp_update_player_chip(ctx);
    mp_cover_show_placeholder(ctx);
    mp_apply_visual(ctx);
    ctx->tick_timer = lv_timer_create(mp_tick_cb, MUSIC_TICK_PERIOD_MS, ctx);

    /* Prime the view from the HA model right away. */
    ha_state_t state = {0};
    if (ha_model_get_state(mp_current_player(ctx), &state)) {
        mp_apply_state_internal(ctx, &state);
    }

    memset(out_instance, 0, sizeof(*out_instance));
    snprintf(out_instance->page_id, sizeof(out_instance->page_id), "%s", config->page_id);
    out_instance->obj = root;
    out_instance->ctx = ctx;
    return ESP_OK;
}
