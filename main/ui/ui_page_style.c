/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_page_style.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
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

/* ---- Dithered page gradients --------------------------------------------
 * LVGL 9 draws bg gradients straight into RGB565 without dithering, so a
 * subtle gradient across the whole content box collapses into a handful of
 * wide bands (red/blue only have 32 levels). The page gradient is therefore
 * rendered once into an RGB565 image with a 4x4 ordered dither and used as the
 * page background image. Pages with the same colours share one buffer. */
#define PAGE_GRAD_CACHE_SIZE 4

typedef struct {
    bool used;
    uint32_t from_rgb;
    uint32_t to_rgb;
    bool horizontal;
    lv_image_dsc_t dsc;
} page_grad_entry_t;

static page_grad_entry_t s_grad_cache[PAGE_GRAD_CACHE_SIZE];

static const uint8_t s_bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/* value: channel in 1/256 steps (0 .. 255*256); bits: 5 or 6. */
static uint32_t ui_page_style_dither_channel(uint32_t value, uint32_t bits, uint8_t threshold)
{
    const uint32_t levels = (1U << bits) - 1U;
    /* Fixed point: scaled = value * levels / 255, in 1/256 steps. */
    const uint32_t scaled = (value * levels) / 255U;
    uint32_t base = scaled >> 8;
    const uint32_t frac = scaled & 0xFFU;
    /* threshold 0..15 -> compare against (t + 0.5) / 16 of a level. */
    if (frac * 16U > ((uint32_t)threshold * 256U + 128U)) {
        base++;
    }
    return base > levels ? levels : base;
}

static const lv_image_dsc_t *ui_page_style_gradient_image(uint32_t from_rgb, uint32_t to_rgb, bool horizontal)
{
    for (size_t i = 0; i < PAGE_GRAD_CACHE_SIZE; i++) {
        page_grad_entry_t *e = &s_grad_cache[i];
        if (e->used && e->from_rgb == from_rgb && e->to_rgb == to_rgb && e->horizontal == horizontal) {
            return &e->dsc;
        }
    }

    page_grad_entry_t *slot = NULL;
    for (size_t i = 0; i < PAGE_GRAD_CACHE_SIZE; i++) {
        if (!s_grad_cache[i].used) {
            slot = &s_grad_cache[i];
            break;
        }
    }
    if (slot == NULL) {
        /* Never evict: a page may still point at any cached image. Callers
         * fall back to LVGL's (banded) gradient instead. */
        return NULL;
    }

    const uint32_t w = (uint32_t)APP_CONTENT_BOX_WIDTH;
    const uint32_t h = (uint32_t)APP_CONTENT_BOX_HEIGHT;
    uint16_t *buf = heap_caps_malloc((size_t)w * h * 2U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG_PAGE_STYLE, "gradient image alloc failed (%ux%u)", (unsigned)w, (unsigned)h);
        return NULL;
    }

    const int32_t r0 = (int32_t)((from_rgb >> 16) & 0xFFU), r1 = (int32_t)((to_rgb >> 16) & 0xFFU);
    const int32_t g0 = (int32_t)((from_rgb >> 8) & 0xFFU), g1 = (int32_t)((to_rgb >> 8) & 0xFFU);
    const int32_t b0 = (int32_t)(from_rgb & 0xFFU), b1 = (int32_t)(to_rgb & 0xFFU);
    const uint32_t span = horizontal ? (w > 1U ? w - 1U : 1U) : (h > 1U ? h - 1U : 1U);

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const int32_t pos = (int32_t)(horizontal ? x : y);
            /* Channel values in 1/256 steps for sub-level precision. */
            const uint32_t r = (uint32_t)((r0 * 256) + ((r1 - r0) * 256 * pos) / (int32_t)span);
            const uint32_t g = (uint32_t)((g0 * 256) + ((g1 - g0) * 256 * pos) / (int32_t)span);
            const uint32_t b = (uint32_t)((b0 * 256) + ((b1 - b0) * 256 * pos) / (int32_t)span);
            const uint8_t t = s_bayer4[y & 3U][x & 3U];
            buf[(size_t)y * w + x] = (uint16_t)((ui_page_style_dither_channel(r, 5, t) << 11) |
                                                (ui_page_style_dither_channel(g, 6, t) << 5) |
                                                ui_page_style_dither_channel(b, 5, t));
        }
    }

    slot->used = true;
    slot->from_rgb = from_rgb;
    slot->to_rgb = to_rgb;
    slot->horizontal = horizontal;
    slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    slot->dsc.header.w = w;
    slot->dsc.header.h = h;
    slot->dsc.header.stride = w * 2U;
    slot->dsc.data_size = w * h * 2U;
    slot->dsc.data = (const uint8_t *)buf;
    return &slot->dsc;
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

    /* LVGL's own gradient is only a fallback for pages (banding, see above). */
    lv_obj_set_style_bg_grad_dir(page_container, LV_GRAD_DIR_NONE, LV_PART_MAIN);

    if (!style->wallpaper) {
        const lv_image_dsc_t *grad = NULL;
        if (strcmp(style->bg_grad_dir, "none") != 0 && ui_tile_style_hex_to_rgb(style->bg_grad_color, &rgb)) {
            const bool horizontal = strcmp(style->bg_grad_dir, "hor") == 0;
            grad = ui_page_style_gradient_image(bg_rgb, rgb, horizontal);
            if (grad == NULL) {
                lv_obj_set_style_bg_grad_color(page_container, lv_color_hex(rgb), LV_PART_MAIN);
                lv_obj_set_style_bg_grad_dir(page_container, horizontal ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER,
                    LV_PART_MAIN);
                lv_obj_set_style_bg_grad_opa(page_container, LV_OPA_COVER, LV_PART_MAIN);
            }
        }
        lv_obj_set_style_bg_image_src(page_container, grad, LV_PART_MAIN);
        if (grad != NULL) {
            lv_obj_set_style_bg_image_opa(page_container, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_image_tiled(page_container, false, LV_PART_MAIN);
            lv_obj_set_style_bg_image_recolor_opa(page_container, LV_OPA_TRANSP, LV_PART_MAIN);
        }
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
