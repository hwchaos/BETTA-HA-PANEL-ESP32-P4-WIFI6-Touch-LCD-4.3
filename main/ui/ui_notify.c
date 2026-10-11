/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#include "ui/ui_notify.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "app_config.h"
#include "drivers/display_init.h"

static const char *TAG = "notify";

#define NOTIFY_DEFAULT_DURATION_MS 10000U
#define NOTIFY_DEFAULT_INTERVAL_MS 500U
#define NOTIFY_MIN_INTERVAL_MS 150U
#define NOTIFY_MAX_DURATION_MS (10U * 60U * 1000U)
#define NOTIFY_MAX_FLASHES 200U
#define NOTIFY_MEASURE_PX 100
#define NOTIFY_MIN_PX 40
#define NOTIFY_GLYPH_CACHE 64
#define NOTIFY_LOCK_MS 1000

/* Poppins, embedded by main/CMakeLists.txt: large text in any size, umlauts
 * included. */
extern const uint8_t poppins_ttf_start[] asm("_binary_Poppins_Regular_ttf_start");
extern const uint8_t poppins_ttf_end[] asm("_binary_Poppins_Regular_ttf_end");

static lv_obj_t *s_overlay;
static lv_obj_t *s_label;
static lv_font_t *s_font;
static lv_timer_t *s_timer;
static bool s_inverted;
static uint32_t s_fg1, s_bg1, s_fg2, s_bg2;
static bool s_blink;           /* false: steady text until the end time */
static uint32_t s_phases_left; /* colour phases still to show (flash mode) */
static int64_t s_end_us;       /* end time (duration mode), 0 = flash mode */

static lv_font_t *notify_font(int32_t px)
{
    return lv_tiny_ttf_create_data_ex(poppins_ttf_start, (size_t)(poppins_ttf_end - poppins_ttf_start), px,
        LV_FONT_KERNING_NONE, NOTIFY_GLYPH_CACHE);
}

static int32_t notify_text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Widest single word, for the wrapped layout. */
static int32_t notify_longest_word_width(const char *text, const lv_font_t *font)
{
    char word[UI_NOTIFY_TEXT_MAX];
    int32_t widest = 0;
    const char *p = text;
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        size_t n = strcspn(p, " ");
        if (n == 0) {
            break;
        }
        if (n >= sizeof(word)) {
            n = sizeof(word) - 1;
        }
        memcpy(word, p, n);
        word[n] = '\0';
        const int32_t w = notify_text_width(word, font);
        if (w > widest) {
            widest = w;
        }
        p += n;
    }
    return widest;
}

static void notify_apply_colors(void)
{
    const lv_color_t bg = lv_color_hex(s_inverted ? s_bg2 : s_bg1);
    const lv_color_t fg = lv_color_hex(s_inverted ? s_fg2 : s_fg1);
    lv_obj_set_style_bg_color(s_overlay, bg, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label, fg, LV_PART_MAIN);
}

/* LVGL context only. */
static void notify_close_locked(void)
{
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_label = NULL;
    }
    /* After the label that used it is gone. */
    if (s_font != NULL) {
        lv_tiny_ttf_destroy(s_font);
        s_font = NULL;
    }
}

static void notify_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (s_overlay == NULL) {
        return;
    }
    if (s_end_us != 0) {
        if (esp_timer_get_time() >= s_end_us) {
            notify_close_locked();
            return;
        }
    } else {
        if (s_phases_left == 0) {
            notify_close_locked();
            return;
        }
        s_phases_left--;
    }
    if (s_blink) {
        s_inverted = !s_inverted;
        notify_apply_colors();
    }
    /* Keeps the screensaver away while the message is up. */
    display_note_activity_from("notify");
}

static void notify_click_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    notify_close_locked();
}

/* "#RRGGBB", "RRGGBB" or a name (English or German). */
static bool notify_parse_color(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    static const struct {
        const char *name;
        uint32_t rgb;
    } names[] = {
        {"white", 0xFFFFFF}, {"weiss", 0xFFFFFF}, {"weiß", 0xFFFFFF}, {"black", 0x000000},
        {"schwarz", 0x000000}, {"red", 0xFF0000}, {"rot", 0xFF0000}, {"green", 0x00C853},
        {"gruen", 0x00C853}, {"grün", 0x00C853}, {"blue", 0x2962FF}, {"blau", 0x2962FF},
        {"yellow", 0xFFD600}, {"gelb", 0xFFD600}, {"orange", 0xFF6D00}, {"purple", 0xAA00FF},
        {"lila", 0xAA00FF}, {"cyan", 0x00E5FF}, {"grey", 0x808080}, {"gray", 0x808080},
        {"grau", 0x808080},
    };
    const char *v = item->valuestring;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(v, names[i].name) == 0) {
            *out = names[i].rgb;
            return true;
        }
    }
    if (*v == '#') {
        v++;
    }
    if (strlen(v) != 6) {
        return false;
    }
    uint32_t rgb = 0;
    for (int i = 0; i < 6; i++) {
        const char c = v[i];
        if (!isxdigit((unsigned char)c)) {
            return false;
        }
        rgb = (rgb << 4) | (uint32_t)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
    }
    *out = rgb;
    return true;
}

esp_err_t ui_notify_parse(const char *payload, ui_notify_req_t *out, bool *stop)
{
    if (payload == NULL || out == NULL || stop == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->duration_ms = NOTIFY_DEFAULT_DURATION_MS;
    out->interval_ms = NOTIFY_DEFAULT_INTERVAL_MS;
    out->fg1 = 0xFFFFFF;
    out->bg1 = 0x000000;
    *stop = false;
    bool have_fg2 = false;
    bool have_bg2 = false;

    while (isspace((unsigned char)*payload)) {
        payload++;
    }
    if (*payload != '{') {
        /* Plain text, e.g. from the Home Assistant text entity. */
        strlcpy(out->text, payload, sizeof(out->text));
    } else {
        cJSON *root = cJSON_Parse(payload);
        if (root == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "stop"))) {
            *stop = true;
        }
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL) {
            strlcpy(out->text, text->valuestring, sizeof(out->text));
        }
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration");
        if (cJSON_IsNumber(duration) && duration->valuedouble > 0) {
            out->duration_ms = (uint32_t)(duration->valuedouble * 1000.0);
        }
        const cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "interval");
        if (cJSON_IsNumber(interval) && interval->valuedouble >= 0) {
            out->interval_ms = (uint32_t)interval->valuedouble;
        }
        const cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "count");
        if (cJSON_IsNumber(count) && count->valuedouble > 0) {
            out->flashes = (uint32_t)count->valuedouble;
        }
        (void)notify_parse_color(cJSON_GetObjectItemCaseSensitive(root, "color"), &out->fg1);
        (void)notify_parse_color(cJSON_GetObjectItemCaseSensitive(root, "background"), &out->bg1);
        have_fg2 = notify_parse_color(cJSON_GetObjectItemCaseSensitive(root, "color2"), &out->fg2);
        have_bg2 = notify_parse_color(cJSON_GetObjectItemCaseSensitive(root, "background2"), &out->bg2);
        cJSON_Delete(root);
    }
    /* Trim trailing whitespace / newlines. */
    size_t len = strlen(out->text);
    while (len > 0 && isspace((unsigned char)out->text[len - 1])) {
        out->text[--len] = '\0';
    }
    if (out->text[0] == '\0') {
        *stop = true;
    }
    /* Blink phase: the first one swapped unless given. */
    if (!have_fg2) {
        out->fg2 = out->bg1;
    }
    if (!have_bg2) {
        out->bg2 = out->fg1;
    }
    if (out->duration_ms > NOTIFY_MAX_DURATION_MS) {
        out->duration_ms = NOTIFY_MAX_DURATION_MS;
    }
    if (out->interval_ms != 0 && out->interval_ms < NOTIFY_MIN_INTERVAL_MS) {
        out->interval_ms = NOTIFY_MIN_INTERVAL_MS;
    }
    if (out->flashes > NOTIFY_MAX_FLASHES) {
        out->flashes = NOTIFY_MAX_FLASHES;
    }
    return ESP_OK;
}

esp_err_t ui_notify_show(const ui_notify_req_t *req)
{
    if (req == NULL || req->text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Wake first: lights the panel and takes down the screensaver. */
    display_note_activity_from("notify");
    if (!display_lock(NOTIFY_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    notify_close_locked();

    const int32_t screen_w = APP_SCREEN_WIDTH;
    const int32_t screen_h = APP_SCREEN_HEIGHT;
    const int32_t max_w = screen_w * 92 / 100;

    /* Size: measure at 100 px and scale - text width grows with the pixel
     * size. One line as large as fits; a long text that would get too small
     * wraps at word boundaries instead. */
    bool wrap = false;
    int32_t px = screen_h / 2;
    lv_font_t *probe = notify_font(NOTIFY_MEASURE_PX);
    if (probe != NULL) {
        const int32_t w100 = notify_text_width(req->text, probe);
        if (w100 > 0) {
            const int32_t fit = (int32_t)((int64_t)NOTIFY_MEASURE_PX * max_w / w100);
            px = LV_MIN(px, fit);
        }
        if (px < screen_h / 5 && strchr(req->text, ' ') != NULL) {
            wrap = true;
            const int32_t word100 = notify_longest_word_width(req->text, probe);
            px = screen_h / 4;
            if (word100 > 0) {
                px = LV_MIN(px, (int32_t)((int64_t)NOTIFY_MEASURE_PX * max_w / word100));
            }
        }
        lv_tiny_ttf_destroy(probe);
    }
    px = LV_MAX(px, NOTIFY_MIN_PX);
    s_font = notify_font(px);
    if (s_font == NULL) {
        display_unlock();
        ESP_LOGW(TAG, "font at %d px not available", (int)px);
        return ESP_ERR_NO_MEM;
    }

    s_overlay = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, screen_w, screen_h);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, notify_click_cb, LV_EVENT_CLICKED, NULL);

    s_label = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_label, s_font, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    if (wrap) {
        lv_obj_set_width(s_label, max_w);
        lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    } else {
        lv_label_set_long_mode(s_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_label, LV_SIZE_CONTENT);
    }
    lv_label_set_text(s_label, req->text);
    lv_obj_center(s_label);

    s_fg1 = req->fg1;
    s_bg1 = req->bg1;
    s_fg2 = req->fg2;
    s_bg2 = req->bg2;
    s_inverted = false;
    notify_apply_colors();

    if (req->flashes > 0 && req->interval_ms > 0) {
        /* Black phase now, then flashes x (inverted + normal). */
        s_end_us = 0;
        s_phases_left = req->flashes * 2;
    } else {
        s_end_us = esp_timer_get_time() + (int64_t)req->duration_ms * 1000;
        s_phases_left = 0;
    }
    /* Without blinking the timer only watches the end time. */
    s_blink = req->interval_ms > 0;
    s_timer = lv_timer_create(notify_timer_cb, s_blink ? req->interval_ms : 1000U, NULL);
    display_unlock();
    ESP_LOGI(TAG, "message \"%s\" (%d px%s, %s)", req->text, (int)px, wrap ? ", wrapped" : "",
        req->flashes > 0 ? "count" : "duration");
    return ESP_OK;
}

void ui_notify_stop(void)
{
    if (!display_lock(NOTIFY_LOCK_MS)) {
        return;
    }
    notify_close_locked();
    display_unlock();
}
