/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_page_style.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ui/theme/theme_default.h"
#include "ui/ui_screen_saver.h"
#include "ui/ui_tile_style.h"

#define TAG_PAGE_STYLE "page_style"

#define PAGE_DIM_MAX 90

/* Pages that paint the wallpaper plus the darkening factor they were built
 * with, so the wallpaper can be re-pointed without losing their look. */
typedef struct {
    lv_obj_t *page;
    int dim;
} page_wallpaper_entry_t;

/* Cropped RGB565 view of the screensaver wallpaper covering exactly the content
 * box, so a page background image lands 1:1 without any offset. Rebuilt from
 * the screensaver buffer whenever the wallpaper changes. */
static lv_image_dsc_t s_wallpaper_view;
static bool s_wallpaper_view_ready;

static page_wallpaper_entry_t s_wallpaper_pages[APP_MAX_PAGES];
static size_t s_wallpaper_page_count;

void ui_page_style_init(ui_page_style_t *style)
{
    if (style == NULL) {
        return;
    }
    memset(style, 0, sizeof(*style));
    snprintf(style->bg_grad_dir, sizeof(style->bg_grad_dir), "%s", "none");
}

bool ui_page_style_is_empty(const ui_page_style_t *style)
{
    if (style == NULL) {
        return true;
    }
    return style->bg_color[0] == '\0' && style->bg_grad_color[0] == '\0' && !style->wallpaper && style->dim <= 0;
}

void ui_page_style_reset(void)
{
    memset(s_wallpaper_pages, 0, sizeof(s_wallpaper_pages));
    s_wallpaper_page_count = 0;
}

static int ui_page_style_clamp_dim(int dim)
{
    if (dim < 0) {
        return 0;
    }
    if (dim > PAGE_DIM_MAX) {
        return PAGE_DIM_MAX;
    }
    return dim;
}

/* Builds the content-box crop of the current wallpaper frame. Returns false
 * when no usable wallpaper is loaded. */
static bool ui_page_style_refresh_view(void)
{
    s_wallpaper_view_ready = false;
    memset(&s_wallpaper_view, 0, sizeof(s_wallpaper_view));

    const lv_image_dsc_t *full = ui_screen_saver_wallpaper_dsc();
    if (full == NULL || full->data == NULL || full->header.stride == 0U) {
        return false;
    }
    if (full->header.w < (uint32_t)(APP_CONTENT_BOX_X + APP_CONTENT_BOX_WIDTH) ||
        full->header.h < (uint32_t)(APP_CONTENT_BOX_Y + APP_CONTENT_BOX_HEIGHT)) {
        ESP_LOGW(TAG_PAGE_STYLE,
            "wallpaper %ux%u too small for content box %dx%d",
            (unsigned)full->header.w,
            (unsigned)full->header.h,
            APP_CONTENT_BOX_WIDTH,
            APP_CONTENT_BOX_HEIGHT);
        return false;
    }

    s_wallpaper_view.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_wallpaper_view.header.cf = LV_COLOR_FORMAT_RGB565;
    s_wallpaper_view.header.flags = 0;
    s_wallpaper_view.header.w = (uint32_t)APP_CONTENT_BOX_WIDTH;
    s_wallpaper_view.header.h = (uint32_t)APP_CONTENT_BOX_HEIGHT;
    s_wallpaper_view.header.stride = (uint32_t)APP_CONTENT_BOX_WIDTH * 2U;
    s_wallpaper_view.data_size = (uint32_t)(APP_CONTENT_BOX_WIDTH * APP_CONTENT_BOX_HEIGHT) * 2U;
    s_wallpaper_view.data =
        full->data + ((size_t)APP_CONTENT_BOX_Y * full->header.stride) + ((size_t)APP_CONTENT_BOX_X * 2U);
    s_wallpaper_view_ready = true;
    return true;
}

static void ui_page_style_apply_wallpaper(lv_obj_t *page_container, int dim)
{
    if (!s_wallpaper_view_ready) {
        lv_obj_set_style_bg_image_src(page_container, NULL, LV_PART_MAIN);
        return;
    }

    lv_obj_set_style_bg_image_src(page_container, &s_wallpaper_view, LV_PART_MAIN);
    lv_obj_set_style_bg_image_opa(page_container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_image_tiled(page_container, false, LV_PART_MAIN);
    /* The darkening layer is a black recolor on top of the image itself. */
    lv_obj_set_style_bg_image_recolor(page_container, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_image_recolor_opa(
        page_container, (lv_opa_t)((ui_page_style_clamp_dim(dim) * 255) / 100), LV_PART_MAIN);
    lv_obj_invalidate(page_container);
}

static void ui_page_style_track(lv_obj_t *page_container, int dim)
{
    for (size_t i = 0; i < s_wallpaper_page_count; i++) {
        if (s_wallpaper_pages[i].page == page_container) {
            s_wallpaper_pages[i].dim = ui_page_style_clamp_dim(dim);
            return;
        }
    }
    if (s_wallpaper_page_count < APP_MAX_PAGES) {
        s_wallpaper_pages[s_wallpaper_page_count].page = page_container;
        s_wallpaper_pages[s_wallpaper_page_count].dim = ui_page_style_clamp_dim(dim);
        s_wallpaper_page_count++;
    }
}

void ui_page_style_apply(lv_obj_t *page_container, const ui_page_style_t *style)
{
    if (page_container == NULL || ui_page_style_is_empty(style)) {
        return;
    }

    uint32_t rgb;
    uint32_t bg_rgb = APP_UI_COLOR_CONTENT_BG;
    if (ui_tile_style_hex_to_rgb(style->bg_color, &rgb)) {
        bg_rgb = rgb;
    }
    lv_obj_set_style_bg_color(page_container, lv_color_hex(bg_rgb), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page_container, LV_OPA_COVER, LV_PART_MAIN);

    if (ui_tile_style_hex_to_rgb(style->bg_grad_color, &rgb)) {
        lv_grad_dir_t dir = LV_GRAD_DIR_VER;
        if (strcmp(style->bg_grad_dir, "hor") == 0) {
            dir = LV_GRAD_DIR_HOR;
        } else if (strcmp(style->bg_grad_dir, "none") == 0) {
            dir = LV_GRAD_DIR_NONE;
        }
        lv_obj_set_style_bg_grad_color(page_container, lv_color_hex(rgb), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(page_container, dir, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_opa(page_container, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_grad_dir(page_container, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    }

    if (!style->wallpaper) {
        lv_obj_set_style_bg_image_src(page_container, NULL, LV_PART_MAIN);
        return;
    }

    /* The wallpaper may not be loaded yet (pages are built before the
     * screensaver has read the file): track the page anyway so a later
     * ui_page_style_reload_wallpaper() can light it up. */
    (void)ui_page_style_refresh_view();
    ui_page_style_track(page_container, style->dim);
    ui_page_style_apply_wallpaper(page_container, style->dim);
}

void ui_page_style_reload_wallpaper(void)
{
    (void)ui_page_style_refresh_view();

    size_t kept = 0;
    for (size_t i = 0; i < s_wallpaper_page_count; i++) {
        lv_obj_t *page_container = s_wallpaper_pages[i].page;
        if (page_container == NULL || !lv_obj_is_valid(page_container)) {
            continue;
        }
        ui_page_style_apply_wallpaper(page_container, s_wallpaper_pages[i].dim);
        s_wallpaper_pages[kept] = s_wallpaper_pages[i];
        kept++;
    }
    s_wallpaper_page_count = kept;
}
