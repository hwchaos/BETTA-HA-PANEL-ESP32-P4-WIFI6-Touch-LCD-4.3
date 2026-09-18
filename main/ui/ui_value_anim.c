/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_value_anim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "diag/system_log.h"
#include "util/log_tags.h"

/* Shortest text we still want to animate to; "--" and empty values are set
 * without an effect because there is nothing to interpolate. */
#define VALUE_ANIM_MIN_TEXT 1U
/* Room for " " plus a unit, e.g. " °C" or " kWh". */
#define VALUE_ANIM_AFFIX_MAX 12U
#define VALUE_ANIM_TEXT_MAX 32U
#define VALUE_ANIM_DECIMALS_MAX 2U
/* Slide effect: how far the new value travels before it settles. */
#define VALUE_ANIM_SLIDE_PX 10
/* The counter keeps one context per running animation; only the few values on
 * the visible page can animate at the same time. */
#define VALUE_ANIM_MAX_RUNNING 6U
/* Counter values travel as thousandths so one animation step stays an integer
 * without losing the decimal place. */
#define VALUE_ANIM_SCALE 1000.0

typedef enum {
    VALUE_ANIM_NONE = 0,
    VALUE_ANIM_FADE,
    VALUE_ANIM_SLIDE,
    VALUE_ANIM_COUNT,
} value_anim_kind_t;

typedef struct {
    lv_obj_t *label;
    char prefix[VALUE_ANIM_AFFIX_MAX];
    char suffix[VALUE_ANIM_AFFIX_MAX];
    char final_text[VALUE_ANIM_TEXT_MAX];
    uint8_t decimals;
} value_anim_ctx_t;

static value_anim_kind_t s_kind = VALUE_ANIM_COUNT;
static uint16_t s_duration_ms = APP_DISPLAY_VALUE_ANIM_DEFAULT_MS;
static value_anim_ctx_t s_ctx[VALUE_ANIM_MAX_RUNNING];
static bool s_settings_loaded;

static value_anim_kind_t kind_from_name(const char *name)
{
    if (name == NULL) {
        return VALUE_ANIM_NONE;
    }
    if (strcmp(name, "fade") == 0) {
        return VALUE_ANIM_FADE;
    }
    if (strcmp(name, "slide") == 0) {
        return VALUE_ANIM_SLIDE;
    }
    if (strcmp(name, "count") == 0) {
        return VALUE_ANIM_COUNT;
    }
    return VALUE_ANIM_NONE;
}

const char *ui_value_anim_mode(void)
{
    switch (s_kind) {
    case VALUE_ANIM_FADE:
        return "fade";
    case VALUE_ANIM_SLIDE:
        return "slide";
    case VALUE_ANIM_COUNT:
        return "count";
    case VALUE_ANIM_NONE:
    default:
        return "none";
    }
}

uint16_t ui_value_anim_duration_ms(void)
{
    return s_duration_ms;
}

bool ui_value_anim_is_running(void)
{
    for (size_t i = 0; i < VALUE_ANIM_MAX_RUNNING; i++) {
        if (s_ctx[i].label != NULL) {
            return true;
        }
    }
    return false;
}

static void animation_start(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb, int32_t from, int32_t to)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, exec_cb);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, s_duration_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

static void fade_exec_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, LV_PART_MAIN);
}

static void slide_exec_cb(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)value, LV_PART_MAIN);
}

static value_anim_ctx_t *ctx_acquire(lv_obj_t *label)
{
    for (size_t i = 0; i < VALUE_ANIM_MAX_RUNNING; i++) {
        if (s_ctx[i].label == label) {
            return &s_ctx[i];
        }
    }
    for (size_t i = 0; i < VALUE_ANIM_MAX_RUNNING; i++) {
        if (s_ctx[i].label == NULL) {
            return &s_ctx[i];
        }
    }
    return NULL;
}

static void count_render(value_anim_ctx_t *ctx, double value)
{
    if (ctx == NULL || ctx->label == NULL) {
        return;
    }
    lv_label_set_text_fmt(ctx->label, "%s%.*f%s", ctx->prefix, (int)ctx->decimals, value, ctx->suffix);
}

static void count_exec_cb(lv_anim_t *anim, int32_t value)
{
    count_render((value_anim_ctx_t *)lv_anim_get_user_data(anim), (double)value / VALUE_ANIM_SCALE);
}

static void count_completed_cb(lv_anim_t *anim)
{
    value_anim_ctx_t *ctx = (value_anim_ctx_t *)lv_anim_get_user_data(anim);
    if (ctx == NULL || ctx->label == NULL) {
        return;
    }
    /* The interpolated value can round differently on its last frame, so the
     * exact text always wins. */
    lv_label_set_text(ctx->label, ctx->final_text);
}

static void count_deleted_cb(lv_anim_t *anim)
{
    value_anim_ctx_t *ctx = (value_anim_ctx_t *)lv_anim_get_user_data(anim);
    if (ctx == NULL) {
        return;
    }
    ctx->label = NULL;
}

/* Splits "22.5 °C" into the prefix "" , the number 22.5 and the suffix " °C" so
 * the counter can move the number while the unit stays put. */
static bool text_split_number(const char *text,
                              char *prefix,
                              size_t prefix_size,
                              double *value,
                              uint8_t *decimals,
                              char *suffix,
                              size_t suffix_size)
{
    if (text == NULL || prefix == NULL || value == NULL || decimals == NULL || suffix == NULL) {
        return false;
    }

    const char *start = NULL;
    for (const char *p = text; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            start = p;
            break;
        }
        if ((*p == '-' || *p == '+') && p[1] >= '0' && p[1] <= '9') {
            start = p;
            break;
        }
        if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
            start = p;
            break;
        }
    }
    if (start == NULL) {
        return false;
    }

    char *end = NULL;
    double parsed = strtod(start, &end);
    if (end == start || !isfinite(parsed)) {
        return false;
    }

    const size_t prefix_len = (size_t)(start - text);
    const size_t suffix_len = strlen(end);
    if (prefix_len + 1U > prefix_size || suffix_len + 1U > suffix_size) {
        return false;
    }

    const char *dot = NULL;
    for (const char *p = start; p < end; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    uint8_t dec = 0;
    if (dot != NULL) {
        for (const char *q = dot + 1; q < end; q++) {
            dec++;
        }
    }
    if (dec > VALUE_ANIM_DECIMALS_MAX) {
        dec = VALUE_ANIM_DECIMALS_MAX;
    }

    memcpy(prefix, text, prefix_len);
    prefix[prefix_len] = '\0';
    memcpy(suffix, end, suffix_len);
    suffix[suffix_len] = '\0';
    *value = parsed;
    *decimals = dec;
    return true;
}

/* Starts the counter; returns false when the two texts cannot be interpolated
 * and the caller should fall back to a plain effect. */
static bool count_start(lv_obj_t *label, const char *old_text, const char *new_text)
{
    if (old_text == NULL || old_text[0] == '\0') {
        return false;
    }

    char old_prefix[VALUE_ANIM_AFFIX_MAX] = {0};
    char old_suffix[VALUE_ANIM_AFFIX_MAX] = {0};
    char new_prefix[VALUE_ANIM_AFFIX_MAX] = {0};
    char new_suffix[VALUE_ANIM_AFFIX_MAX] = {0};
    double old_value = 0.0;
    double new_value = 0.0;
    uint8_t old_decimals = 0;
    uint8_t new_decimals = 0;

    if (!text_split_number(old_text, old_prefix, sizeof(old_prefix), &old_value, &old_decimals,
                           old_suffix, sizeof(old_suffix)) ||
        !text_split_number(new_text, new_prefix, sizeof(new_prefix), &new_value, &new_decimals,
                           new_suffix, sizeof(new_suffix))) {
        return false;
    }
    /* Only the digits may change, otherwise the unit or the sign would appear to
     * move while counting. */
    if (strcmp(old_prefix, new_prefix) != 0 || strcmp(old_suffix, new_suffix) != 0) {
        return false;
    }
    if (old_value == new_value || fabs(old_value) > 1000000.0 || fabs(new_value) > 1000000.0) {
        return false;
    }

    value_anim_ctx_t *ctx = ctx_acquire(label);
    if (ctx == NULL) {
        return false;
    }

    uint8_t decimals = (old_decimals > new_decimals) ? old_decimals : new_decimals;
    strlcpy(ctx->prefix, new_prefix, sizeof(ctx->prefix));
    strlcpy(ctx->suffix, new_suffix, sizeof(ctx->suffix));
    strlcpy(ctx->final_text, new_text, sizeof(ctx->final_text));
    ctx->decimals = decimals;
    ctx->label = label;

    /* Show the starting value immediately: waiting for the first tick would
     * leave the previous number on screen for a frame. */
    count_render(ctx, old_value);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, label);
    lv_anim_set_custom_exec_cb(&anim, count_exec_cb);
    lv_anim_set_values(&anim, (int32_t)(old_value * VALUE_ANIM_SCALE),
                       (int32_t)(new_value * VALUE_ANIM_SCALE));
    lv_anim_set_duration(&anim, s_duration_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_user_data(&anim, ctx);
    lv_anim_set_completed_cb(&anim, count_completed_cb);
    lv_anim_set_deleted_cb(&anim, count_deleted_cb);
    lv_anim_start(&anim);
    return true;
}

static void settings_load(void)
{
    runtime_settings_t settings;
    runtime_settings_set_defaults(&settings);
    if (runtime_settings_load(&settings) != ESP_OK) {
        runtime_settings_set_defaults(&settings);
    }
    ui_value_anim_apply_settings(&settings);
}

static void settings_ensure_loaded(void)
{
    if (!s_settings_loaded) {
        settings_load();
    }
}

void ui_value_anim_init(void)
{
    settings_load();
}

void ui_value_anim_apply_settings(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    const value_anim_kind_t kind = kind_from_name(settings->display_value_anim);
    uint16_t duration = settings->display_value_anim_ms;
    if (duration > APP_DISPLAY_VALUE_ANIM_MAX_MS) {
        duration = APP_DISPLAY_VALUE_ANIM_MAX_MS;
    }
    const bool changed = !s_settings_loaded || kind != s_kind || duration != s_duration_ms;
    if (!changed) {
        return;
    }

    s_settings_loaded = true;
    s_kind = kind;
    s_duration_ms = duration;
    system_log_write_info(TAG_VALUE_ANIM, "value animation '%s' %u ms", ui_value_anim_mode(),
                          (unsigned)s_duration_ms);
}

void ui_value_anim_set_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    if (text == NULL) {
        text = "";
    }

    settings_ensure_loaded();

    const char *old_text = lv_label_get_text(label);
    if (old_text == NULL) {
        old_text = "";
    }
    if (strcmp(old_text, text) == 0) {
        return;
    }

    /* An effect that is still running would fight the new one. */
    lv_anim_delete(label, NULL);

    if (s_kind == VALUE_ANIM_NONE || s_duration_ms == 0 || strlen(text) < VALUE_ANIM_MIN_TEXT) {
        lv_label_set_text(label, text);
        return;
    }

    if (s_kind == VALUE_ANIM_COUNT && count_start(label, old_text, text)) {
        return;
    }

    /* Fade, slide, and the counter fallback for texts that hold no single
     * number (e.g. "--" or a word). */
    lv_label_set_text(label, text);
    if (s_kind == VALUE_ANIM_SLIDE) {
        lv_obj_set_style_translate_y(label, VALUE_ANIM_SLIDE_PX, LV_PART_MAIN);
        animation_start(label, slide_exec_cb, VALUE_ANIM_SLIDE_PX, 0);
    } else {
        lv_obj_set_style_opa(label, LV_OPA_TRANSP, LV_PART_MAIN);
        animation_start(label, fade_exec_cb, LV_OPA_TRANSP, LV_OPA_COVER);
    }
}
