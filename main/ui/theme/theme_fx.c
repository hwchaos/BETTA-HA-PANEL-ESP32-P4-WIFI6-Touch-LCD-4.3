/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#include "ui/theme/theme_fx.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_config.h"
#include "ui/theme/theme_default.h"
#include "ui/theme/theme_palette.h"

#define TAG_THEME_FX "theme_fx"

/* Night-blue backdrop with a teal glow top left and a violet glow bottom
 * right; translucent tiles, "on" tiles glow teal. */
static const theme_fx_t k_fx_aurora = {
    .backdrop_top = 0x0A1022,
    .backdrop_bottom = 0x070A16,
    .glows = {
        {.color = 0x2DD4BF, .cx_pct = 12, .cy_pct = 18, .radius_pct = 58, .strength = 92},
        {.color = 0xA855F7, .cx_pct = 88, .cy_pct = 85, .radius_pct = 55, .strength = 98},
    },
    .bar_opa = LV_OPA_70,
    .card_opa = LV_OPA_70,
    .card_edge = 0xFFFFFF,
    .card_edge_opa = LV_OPA_10,
    .card_radius = 20,
    .on_from = 0x1B4F5C,
    .on_to = 0x2A2C6E,
    .on_edge = 0x5EEAD4,
    .on_edge_opa = LV_OPA_70,
    .on_glow = 0x5EEAD4,
    .on_glow_opa = LV_OPA_40,
    .on_glow_width = 26,
};

/* Aurora Glow: Aurora whose glowing tiles breathe. */
static const theme_fx_t k_fx_aurora_glow = {
    .backdrop_top = 0x0A1022,
    .backdrop_bottom = 0x070A16,
    .glows = {
        {.color = 0x2DD4BF, .cx_pct = 12, .cy_pct = 18, .radius_pct = 58, .strength = 92},
        {.color = 0xA855F7, .cx_pct = 88, .cy_pct = 85, .radius_pct = 55, .strength = 98},
    },
    .bar_opa = LV_OPA_70,
    .card_opa = LV_OPA_70,
    .card_edge = 0xFFFFFF,
    .card_edge_opa = LV_OPA_10,
    .card_radius = 20,
    .on_from = 0x1B4F5C,
    .on_to = 0x2A2C6E,
    .on_edge = 0x5EEAD4,
    .on_edge_opa = LV_OPA_70,
    .on_glow = 0x5EEAD4,
    .on_glow_opa = LV_OPA_40,
    .on_glow_width = 26,
    .breathe_ms = 3200,
};

/* Warm graphite with a faint copper haze; "on" tiles turn copper. */
static const theme_fx_t k_fx_graphite_copper = {
    .backdrop_top = 0x18171B,
    .backdrop_bottom = 0x0E0D10,
    .glows = {
        {.color = 0xC87A4A, .cx_pct = 85, .cy_pct = 10, .radius_pct = 55, .strength = 34},
        {.color = 0x5A4A6A, .cx_pct = 10, .cy_pct = 95, .radius_pct = 50, .strength = 30},
    },
    .bar_opa = LV_OPA_80,
    .card_opa = LV_OPA_90,
    .card_edge = 0xFFFFFF,
    .card_edge_opa = LV_OPA_10,
    .card_radius = 18,
    .on_from = 0xB8734A,
    .on_to = 0x7A4126,
    .on_edge = 0xF0B48C,
    .on_edge_opa = LV_OPA_60,
    .on_glow = 0xD08A5C,
    .on_glow_opa = LV_OPA_40,
    .on_glow_width = 24,
};

const theme_fx_t *theme_fx_active(void)
{
    const char *id = theme_palette_active_id();
    if (id == NULL) {
        return NULL;
    }
    if (strcmp(id, "aurora") == 0) {
        return &k_fx_aurora;
    }
    if (strcmp(id, "aurora_glow") == 0) {
        return &k_fx_aurora_glow;
    }
    if (strcmp(id, "graphite_copper") == 0) {
        return &k_fx_graphite_copper;
    }
    return NULL;
}

/* ---- Backdrop -------------------------------------------------------------- */

typedef struct {
    const theme_fx_t *fx;
    lv_image_dsc_t dsc;
} fx_backdrop_t;

/* One slot per effect theme: a page rebuilt for another theme may still hold
 * the old image until it is torn down, so buffers are never freed. */
static fx_backdrop_t s_backdrops[2];

static const uint8_t s_bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/* value: channel in 1/256 steps (0 .. 255*256). */
static uint32_t fx_dither(uint32_t value, uint32_t bits, uint8_t threshold)
{
    const uint32_t levels = (1U << bits) - 1U;
    const uint32_t scaled = (value * levels) / 255U;
    uint32_t base = scaled >> 8;
    if ((scaled & 0xFFU) * 16U > ((uint32_t)threshold * 256U + 128U)) {
        base++;
    }
    return base > levels ? levels : base;
}

static bool fx_render_backdrop(const theme_fx_t *fx, lv_image_dsc_t *dsc)
{
    const uint32_t w = (uint32_t)APP_SCREEN_WIDTH;
    const uint32_t h = (uint32_t)APP_SCREEN_HEIGHT;
    uint16_t *buf = heap_caps_malloc((size_t)w * h * 2U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG_THEME_FX, "backdrop alloc failed (%ux%u)", (unsigned)w, (unsigned)h);
        return false;
    }

    const float top[3] = {(float)((fx->backdrop_top >> 16) & 0xFF), (float)((fx->backdrop_top >> 8) & 0xFF),
                          (float)(fx->backdrop_top & 0xFF)};
    const float bot[3] = {(float)((fx->backdrop_bottom >> 16) & 0xFF), (float)((fx->backdrop_bottom >> 8) & 0xFF),
                          (float)(fx->backdrop_bottom & 0xFF)};
    float gx[2], gy[2], gr[2], gs[2], gc[2][3];
    for (int g = 0; g < 2; g++) {
        const theme_fx_glow_t *glow = &fx->glows[g];
        gx[g] = (float)w * (float)glow->cx_pct / 100.0f;
        gy[g] = (float)h * (float)glow->cy_pct / 100.0f;
        gr[g] = (float)w * (float)glow->radius_pct / 100.0f;
        gs[g] = (float)glow->strength / 255.0f;
        gc[g][0] = (float)((glow->color >> 16) & 0xFF);
        gc[g][1] = (float)((glow->color >> 8) & 0xFF);
        gc[g][2] = (float)(glow->color & 0xFF);
    }

    for (uint32_t y = 0; y < h; y++) {
        const float t = (h > 1U) ? (float)y / (float)(h - 1U) : 0.0f;
        float base[3];
        for (int c = 0; c < 3; c++) {
            base[c] = top[c] + (bot[c] - top[c]) * t;
        }
        for (uint32_t x = 0; x < w; x++) {
            float px[3] = {base[0], base[1], base[2]};
            for (int g = 0; g < 2; g++) {
                if (gs[g] <= 0.0f || gr[g] <= 0.0f) {
                    continue;
                }
                const float dx = (float)x - gx[g];
                const float dy = (float)y - gy[g];
                const float d = sqrtf(dx * dx + dy * dy) / gr[g];
                if (d >= 1.0f) {
                    continue;
                }
                /* Smooth falloff (smoothstep of 1-d), blended towards the glow
                 * colour like a soft light over the base. */
                const float k = 1.0f - d;
                const float a = gs[g] * k * k * (3.0f - 2.0f * k);
                for (int c = 0; c < 3; c++) {
                    px[c] += (gc[g][c] - px[c]) * a;
                }
            }
            const uint8_t th = s_bayer4[y & 3U][x & 3U];
            uint32_t r = (uint32_t)(px[0] * 256.0f);
            uint32_t gch = (uint32_t)(px[1] * 256.0f);
            uint32_t b = (uint32_t)(px[2] * 256.0f);
            if (r > 255U * 256U) r = 255U * 256U;
            if (gch > 255U * 256U) gch = 255U * 256U;
            if (b > 255U * 256U) b = 255U * 256U;
            buf[(size_t)y * w + x] =
                (uint16_t)((fx_dither(r, 5, th) << 11) | (fx_dither(gch, 6, th) << 5) | fx_dither(b, 5, th));
        }
    }

    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2U;
    dsc->data_size = w * h * 2U;
    dsc->data = (const uint8_t *)buf;
    return true;
}

const lv_image_dsc_t *theme_fx_backdrop(void)
{
    const theme_fx_t *fx = theme_fx_active();
    if (fx == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_backdrops) / sizeof(s_backdrops[0]); i++) {
        if (s_backdrops[i].fx == fx) {
            return &s_backdrops[i].dsc;
        }
    }
    for (size_t i = 0; i < sizeof(s_backdrops) / sizeof(s_backdrops[0]); i++) {
        if (s_backdrops[i].fx == NULL) {
            if (!fx_render_backdrop(fx, &s_backdrops[i].dsc)) {
                return NULL;
            }
            s_backdrops[i].fx = fx;
            return &s_backdrops[i].dsc;
        }
    }
    return NULL;
}

/* ---- Tiles ----------------------------------------------------------------- */

bool theme_fx_tile_is_on(lv_obj_t *tile)
{
    if (tile == NULL) {
        return false;
    }
    const lv_color_t bg = lv_obj_get_style_bg_color(tile, LV_PART_MAIN);
    const uint32_t rgb = lv_color_to_u32(bg) & 0xFFFFFFU;
    if (rgb == (APP_UI_COLOR_CARD_BG_ON & 0xFFFFFFU)) {
        return true;
    }
    /* A tile this pass already turned into the "on" gradient. */
    const theme_fx_t *fx = theme_fx_active();
    return fx != NULL && rgb == (fx->on_from & 0xFFFFFFU);
}

/* Breathing: one anim per glowing tile; v runs 0..255 and back. */
static void theme_fx_breathe_exec(void *obj, int32_t v)
{
    const theme_fx_t *fx = theme_fx_active();
    if (fx == NULL) {
        return;
    }
    lv_obj_t *tile = (lv_obj_t *)obj;
    const int32_t base = fx->on_glow_opa;
    /* glow between ~35 % and ~100 % of the theme's glow, edge follows */
    const int32_t glow = (base * 35) / 100 + ((base * 65) / 100 + 40) * v / 255;
    const int32_t edge = (fx->on_edge_opa * 45) / 100 + ((fx->on_edge_opa * 55) / 100) * v / 255;
    lv_obj_set_style_shadow_opa(tile, (lv_opa_t)(glow > 255 ? 255 : glow), LV_PART_MAIN);
    lv_obj_set_style_border_opa(tile, (lv_opa_t)edge, LV_PART_MAIN);
}

static void theme_fx_breathe(lv_obj_t *tile, const theme_fx_t *fx, bool on)
{
    lv_anim_delete(tile, theme_fx_breathe_exec);
    if (!on || fx == NULL || fx->breathe_ms == 0) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tile);
    lv_anim_set_exec_cb(&a, theme_fx_breathe_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, fx->breathe_ms / 2);
    lv_anim_set_playback_duration(&a, fx->breathe_ms / 2);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    /* desynchronise neighbouring tiles a little */
    lv_anim_set_delay(&a, (uint32_t)(((uintptr_t)tile >> 4) % (fx->breathe_ms / 2 + 1)));
    lv_anim_start(&a);
}

void theme_fx_style_tile(lv_obj_t *tile, bool is_on)
{
    const theme_fx_t *fx = theme_fx_active();
    if (fx == NULL || tile == NULL) {
        return;
    }

    lv_obj_set_style_radius(tile, fx->card_radius, LV_PART_MAIN);
    if (is_on) {
        lv_obj_set_style_bg_color(tile, lv_color_hex(fx->on_from), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(tile, lv_color_hex(fx->on_to), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, lv_color_hex(fx->on_edge), LV_PART_MAIN);
        lv_obj_set_style_border_opa(tile, fx->on_edge_opa, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(tile, fx->on_glow_width, LV_PART_MAIN);
        lv_obj_set_style_shadow_spread(tile, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(tile, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(tile, lv_color_hex(fx->on_glow), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(tile, fx->on_glow_opa, LV_PART_MAIN);
        theme_fx_breathe(tile, fx, true);
    } else {
        theme_fx_breathe(tile, fx, false);
        lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_NONE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tile, fx->card_opa, LV_PART_MAIN);
        lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(tile, lv_color_hex(fx->card_edge), LV_PART_MAIN);
        lv_obj_set_style_border_opa(tile, fx->card_edge_opa, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(tile, 0, LV_PART_MAIN);
    }
}
