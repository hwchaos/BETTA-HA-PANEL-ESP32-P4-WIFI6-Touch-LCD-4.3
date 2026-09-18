/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_tile_style.h"

#include <stddef.h>
#include <string.h>

#include "ui/fonts/app_text_fonts.h"

/* Fonts ordered by visual size. Duplicates (the size macros collapse onto fewer
 * real fonts) are removed at runtime so one scale step always lands on the next
 * font that actually exists in the build. */
static const lv_font_t *const s_font_ladder_src[] = {
    APP_FONT_TEXT_12,
    APP_FONT_TEXT_14,
    APP_FONT_TEXT_16,
    APP_FONT_TEXT_18,
    APP_FONT_TEXT_20,
    APP_FONT_TEXT_22,
    APP_FONT_TEXT_24,
    APP_FONT_TEXT_28,
    APP_FONT_TEXT_34,
    APP_FONT_DISPLAY_36,
    APP_FONT_DISPLAY_38,
    APP_FONT_DISPLAY_40,
};

#define FONT_LADDER_SRC_LEN (sizeof(s_font_ladder_src) / sizeof(s_font_ladder_src[0]))
#define FONT_LADDER_MAX 16U

static const lv_font_t *s_font_ladder[FONT_LADDER_MAX];
static size_t s_font_ladder_len;

static void font_ladder_build(void)
{
    if (s_font_ladder_len > 0U) {
        return;
    }
    for (size_t i = 0; i < FONT_LADDER_SRC_LEN; i++) {
        const lv_font_t *font = s_font_ladder_src[i];
        if (font == NULL) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < s_font_ladder_len; j++) {
            if (s_font_ladder[j] == font) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && s_font_ladder_len < FONT_LADDER_MAX) {
            s_font_ladder[s_font_ladder_len++] = font;
        }
    }
    if (s_font_ladder_len == 0U) {
        s_font_ladder[s_font_ladder_len++] = LV_FONT_DEFAULT;
    }
}

static int font_ladder_index(const lv_font_t *font)
{
    font_ladder_build();
    for (size_t i = 0; i < s_font_ladder_len; i++) {
        if (s_font_ladder[i] == font) {
            return (int)i;
        }
    }
    return -1;
}

static int font_scale_step(const char *scale)
{
    if (scale == NULL) {
        return 0;
    }
    if (strcmp(scale, "s") == 0) {
        return -1;
    }
    if (strcmp(scale, "l") == 0) {
        return 1;
    }
    if (strcmp(scale, "xl") == 0) {
        return 2;
    }
    return 0;
}

bool ui_tile_style_hex_to_rgb(const char *text, uint32_t *out_rgb)
{
    if (text == NULL) {
        return false;
    }
    const char *p = text;
    if (p[0] == '#') {
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (strlen(p) != 6U) {
        return false;
    }
    uint32_t value = 0U;
    for (size_t i = 0; i < 6U; i++) {
        const char c = p[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') {
            digit = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (uint32_t)(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            digit = (uint32_t)(c - 'A') + 10U;
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    *out_rgb = value & 0xFFFFFFU;
    return true;
}

static bool text_uses_icon_glyph(const char *text)
{
    if (text == NULL) {
        return false;
    }
    const unsigned char *p = (const unsigned char *)text;
    while (*p != '\0') {
        uint32_t cp;
        size_t len;
        if (p[0] < 0x80U) {
            cp = p[0];
            len = 1U;
        } else if ((p[0] & 0xE0U) == 0xC0U) {
            cp = (uint32_t)(p[0] & 0x1FU);
            len = 2U;
        } else if ((p[0] & 0xF0U) == 0xE0U) {
            cp = (uint32_t)(p[0] & 0x0FU);
            len = 3U;
        } else if ((p[0] & 0xF8U) == 0xF0U) {
            cp = (uint32_t)(p[0] & 0x07U);
            len = 4U;
        } else {
            p++;
            continue;
        }
        bool valid = true;
        for (size_t i = 1U; i < len; i++) {
            if ((p[i] & 0xC0U) != 0x80U) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (uint32_t)(p[i] & 0x3FU);
        }
        if (!valid) {
            p++;
            continue;
        }
        /* Material Design Icons (U+F0000+) and LV_SYMBOL/FontAwesome (U+E000..U+F8FF). */
        if ((cp >= 0xE000U && cp <= 0xF8FFU) || cp >= 0xF0000U) {
            return true;
        }
        p += len;
    }
    return false;
}

static void apply_font_scale_recursive(lv_obj_t *obj, int step)
{
    const uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if (child == NULL) {
            continue;
        }
        if (lv_obj_has_class(child, &lv_label_class)) {
            if (!text_uses_icon_glyph(lv_label_get_text(child))) {
                const int index = font_ladder_index(lv_obj_get_style_text_font(child, LV_PART_MAIN));
                if (index >= 0) {
                    int target = index + step;
                    if (target < 0) {
                        target = 0;
                    } else if (target >= (int)s_font_ladder_len) {
                        target = (int)s_font_ladder_len - 1;
                    }
                    lv_obj_set_style_text_font(child, s_font_ladder[target], LV_PART_MAIN);
                }
            }
        }
        apply_font_scale_recursive(child, step);
    }
}

/* Colour roles resolved from the style; a role without its own colour falls back
 * to the generic text colour so a tile can mix (e.g. only the value tinted). */
typedef struct {
    bool have_text;
    uint32_t text_rgb;
    bool have_icon;
    uint32_t icon_rgb;
    bool have_title;
    uint32_t title_rgb;
    bool have_label;
    uint32_t label_rgb;
    bool have_value;
    uint32_t value_rgb;
} tile_text_colors_t;

static bool tile_text_color_for_label(lv_obj_t *label, const tile_text_colors_t *colors, bool include_icons,
                                      uint32_t *out_rgb)
{
    if (text_uses_icon_glyph(lv_label_get_text(label))) {
        if (colors->have_icon && include_icons) {
            *out_rgb = colors->icon_rgb;
            return true;
        }
        return false;
    }

    if (lv_obj_has_flag(label, LV_OBJ_FLAG_USER_1) && colors->have_title) {
        *out_rgb = colors->title_rgb;
        return true;
    }
    if (lv_obj_has_flag(label, LV_OBJ_FLAG_USER_2) && colors->have_label) {
        *out_rgb = colors->label_rgb;
        return true;
    }
    if (lv_obj_has_flag(label, LV_OBJ_FLAG_USER_3) && colors->have_value) {
        *out_rgb = colors->value_rgb;
        return true;
    }

    if (colors->have_text) {
        *out_rgb = colors->text_rgb;
        return true;
    }
    return false;
}

static void apply_text_colors_recursive(lv_obj_t *obj, const tile_text_colors_t *colors, bool include_icons)
{
    const uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, (int32_t)i);
        if (child == NULL) {
            continue;
        }
        if (lv_obj_has_class(child, &lv_label_class)) {
            uint32_t rgb = 0;
            if (tile_text_color_for_label(child, colors, include_icons, &rgb)) {
                lv_obj_set_style_text_color(child, lv_color_hex(rgb), LV_PART_MAIN);
            }
        }
        apply_text_colors_recursive(child, colors, include_icons);
    }
}

bool ui_tile_style_is_empty(const ui_tile_style_t *style)
{
    if (style == NULL) {
        return true;
    }
    const bool no_color = (style->bg_color == NULL || style->bg_color[0] == '\0') &&
                          (style->bg_grad_color == NULL || style->bg_grad_color[0] == '\0') &&
                          (style->border_color == NULL || style->border_color[0] == '\0') &&
                          (style->text_color == NULL || style->text_color[0] == '\0') &&
                          (style->title_color == NULL || style->title_color[0] == '\0') &&
                          (style->label_color == NULL || style->label_color[0] == '\0') &&
                          (style->value_color == NULL || style->value_color[0] == '\0') &&
                          (style->icon_color == NULL || style->icon_color[0] == '\0');
    const bool no_scale = (style->font_scale == NULL || style->font_scale[0] == '\0' ||
                           strcmp(style->font_scale, "auto") == 0);
    return no_color && no_scale && style->border_width < 0 && style->radius < 0 && style->opacity < 0 &&
           !style->shadow;
}

void ui_tile_style_apply_bg(lv_obj_t *tile, const ui_tile_style_t *style)
{
    if (tile == NULL || ui_tile_style_is_empty(style)) {
        return;
    }

    uint32_t bg_rgb;
    uint32_t grad_rgb;
    const bool have_bg = ui_tile_style_hex_to_rgb(style->bg_color, &bg_rgb);
    const bool have_grad = ui_tile_style_hex_to_rgb(style->bg_grad_color, &grad_rgb);

    if (have_bg) {
        lv_obj_set_style_bg_color(tile, lv_color_hex(bg_rgb), LV_PART_MAIN);
    }

    if (have_bg || have_grad) {
        if (have_grad) {
            lv_grad_dir_t dir = LV_GRAD_DIR_VER;
            if (style->bg_grad_dir != NULL && strcmp(style->bg_grad_dir, "hor") == 0) {
                dir = LV_GRAD_DIR_HOR;
            } else if (style->bg_grad_dir != NULL && strcmp(style->bg_grad_dir, "none") == 0) {
                dir = LV_GRAD_DIR_NONE;
            }
            lv_obj_set_style_bg_grad_color(tile, lv_color_hex(grad_rgb), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(tile, dir, LV_PART_MAIN);
            lv_obj_set_style_bg_grad_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        } else if (have_bg) {
            lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_NONE, LV_PART_MAIN);
        }
    }

    if (style->opacity >= 0) {
        int opacity = style->opacity;
        if (opacity > 100) {
            opacity = 100;
        }
        lv_obj_set_style_bg_opa(tile, (lv_opa_t)((opacity * 255) / 100), LV_PART_MAIN);
    }
}

static void ui_tile_style_text_colors_resolve(const ui_tile_style_t *style, tile_text_colors_t *colors)
{
    colors->have_text = ui_tile_style_hex_to_rgb(style->text_color, &colors->text_rgb);
    colors->have_icon = ui_tile_style_hex_to_rgb(style->icon_color, &colors->icon_rgb);
    colors->have_title = ui_tile_style_hex_to_rgb(style->title_color, &colors->title_rgb);
    colors->have_label = ui_tile_style_hex_to_rgb(style->label_color, &colors->label_rgb);
    colors->have_value = ui_tile_style_hex_to_rgb(style->value_color, &colors->value_rgb);
}

void ui_tile_style_apply(lv_obj_t *tile, const ui_tile_style_t *style)
{
    if (tile == NULL || ui_tile_style_is_empty(style)) {
        return;
    }

    ui_tile_style_apply_bg(tile, style);

    if (style->radius >= 0) {
        int radius = style->radius;
        if (radius > 128) {
            radius = 128;
        }
        lv_obj_set_style_radius(tile, radius, LV_PART_MAIN);
    }

    uint32_t border_rgb;
    const bool have_border = ui_tile_style_hex_to_rgb(style->border_color, &border_rgb);
    if (have_border) {
        lv_obj_set_style_border_color(tile, lv_color_hex(border_rgb), LV_PART_MAIN);
    }
    if (style->border_width >= 0) {
        int width = style->border_width;
        if (width > 16) {
            width = 16;
        }
        lv_obj_set_style_border_width(tile, width, LV_PART_MAIN);
        if (width > 0) {
            lv_obj_set_style_border_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
        }
    }

    if (style->shadow) {
        lv_obj_set_style_shadow_width(tile, 18, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(tile, 4, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(tile, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(tile, LV_OPA_40, LV_PART_MAIN);
    }

    if (style->font_scale != NULL && style->font_scale[0] != '\0' &&
        strcmp(style->font_scale, "auto") != 0) {
        const int step = font_scale_step(style->font_scale);
        if (step != 0) {
            apply_font_scale_recursive(tile, step);
        }
    }

    tile_text_colors_t colors = {0};
    ui_tile_style_text_colors_resolve(style, &colors);
    if (colors.have_text) {
        lv_obj_set_style_text_color(tile, lv_color_hex(colors.text_rgb), LV_PART_MAIN);
    }
    if (colors.have_text || colors.have_icon || colors.have_title || colors.have_label || colors.have_value) {
        apply_text_colors_recursive(tile, &colors, true);
    }
}

/* Shared body of the two text colour passes: `include_icons` false leaves icon
 * glyphs untouched so a widget can keep the colour it derived from the state. */
static void tile_text_colors_apply(lv_obj_t *tile, const ui_tile_style_t *style, bool include_icons)
{
    if (tile == NULL || style == NULL) {
        return;
    }

    tile_text_colors_t colors = {0};
    ui_tile_style_text_colors_resolve(style, &colors);
    if (!colors.have_text && !colors.have_icon && !colors.have_title && !colors.have_label && !colors.have_value) {
        return;
    }
    if (colors.have_text) {
        lv_obj_set_style_text_color(tile, lv_color_hex(colors.text_rgb), LV_PART_MAIN);
    }
    apply_text_colors_recursive(tile, &colors, include_icons);
}

void ui_tile_style_apply_text_colors(lv_obj_t *tile, const ui_tile_style_t *style)
{
    tile_text_colors_apply(tile, style, true);
}

void ui_tile_style_apply_state_text_colors(lv_obj_t *tile, const ui_tile_style_t *style)
{
    tile_text_colors_apply(tile, style, false);
}
