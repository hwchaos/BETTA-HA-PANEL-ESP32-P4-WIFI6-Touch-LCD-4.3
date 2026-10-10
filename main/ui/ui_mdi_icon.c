/* SPDX-License-Identifier: LicenseRef-FNCL-1.1 */
#include "ui/ui_mdi_icon.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"

#include "ha/ha_model.h"
#include "ui/fonts/mdi_names.h"

#define TAG_MDI_ICON "mdi_icon"

#if LV_USE_TINY_TTF
extern const uint8_t mdi_ttf_start[] asm("_binary_materialdesignicons_webfont_ttf_start");
extern const uint8_t mdi_ttf_end[] asm("_binary_materialdesignicons_webfont_ttf_end");
#endif

/* Fonts per pixel size. Tiles use a handful of sizes, so a small table is
 * enough; each Tiny TTF font keeps its own glyph cache. */
#define MDI_FONT_SLOTS 8
#define MDI_FONT_GLYPH_CACHE 24

typedef struct {
    int32_t px;
    lv_font_t *font;
} mdi_font_slot_t;

static mdi_font_slot_t s_fonts[MDI_FONT_SLOTS];

uint32_t ui_mdi_icon_codepoint(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (strncmp(name, "mdi:", 4) == 0) {
        name += 4;
    }
    size_t lo = 0;
    size_t hi = g_mdi_names_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = strcmp(name, g_mdi_names[mid].name);
        if (cmp == 0) {
            return g_mdi_names[mid].codepoint;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return 0;
}

const lv_font_t *ui_mdi_icon_font(int32_t px)
{
#if LV_USE_TINY_TTF
    if (px < 8) {
        px = 8;
    }
    mdi_font_slot_t *free_slot = NULL;
    for (size_t i = 0; i < MDI_FONT_SLOTS; i++) {
        if (s_fonts[i].font != NULL && s_fonts[i].px == px) {
            return s_fonts[i].font;
        }
        if (s_fonts[i].font == NULL && free_slot == NULL) {
            free_slot = &s_fonts[i];
        }
    }
    if (free_slot == NULL) {
        /* All sizes taken: use the closest one instead of creating more. */
        mdi_font_slot_t *best = &s_fonts[0];
        for (size_t i = 1; i < MDI_FONT_SLOTS; i++) {
            if (LV_ABS(s_fonts[i].px - px) < LV_ABS(best->px - px)) {
                best = &s_fonts[i];
            }
        }
        return best->font;
    }
    lv_font_t *font = lv_tiny_ttf_create_data_ex(mdi_ttf_start, (size_t)(mdi_ttf_end - mdi_ttf_start), px,
        LV_FONT_KERNING_NONE, MDI_FONT_GLYPH_CACHE);
    if (font == NULL) {
        ESP_LOGW(TAG_MDI_ICON, "could not create MDI font at %d px", (int)px);
        return NULL;
    }
    free_slot->px = px;
    free_slot->font = font;
    return font;
#else
    (void)px;
    return NULL;
#endif
}

int32_t ui_mdi_icon_font_px(const lv_font_t *font)
{
    if (font == NULL) {
        return 0;
    }
    for (size_t i = 0; i < MDI_FONT_SLOTS; i++) {
        if (s_fonts[i].font == font) {
            return s_fonts[i].px;
        }
    }
    return 0;
}

static void mdi_utf8(uint32_t cp, char out[5])
{
    out[0] = (char)(0xF0U | ((cp >> 18) & 0x07U));
    out[1] = (char)(0x80U | ((cp >> 12) & 0x3FU));
    out[2] = (char)(0x80U | ((cp >> 6) & 0x3FU));
    out[3] = (char)(0x80U | (cp & 0x3FU));
    out[4] = '\0';
}

bool ui_mdi_icon_apply(lv_obj_t *label, const char *name, int32_t px)
{
    if (label == NULL) {
        return false;
    }
    const uint32_t cp = ui_mdi_icon_codepoint(name);
    if (cp == 0) {
        return false;
    }
    const lv_font_t *font = ui_mdi_icon_font(px);
    if (font == NULL) {
        return false;
    }
    char utf8[5];
    mdi_utf8(cp, utf8);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_label_set_text(label, utf8);
    return true;
}

/* ---- Defaults like the HA frontend ------------------------------------------- */

typedef struct {
    const char *device_class; /* NULL = any */
    const char *on;
    const char *off;
} mdi_class_icon_t;

static const char *mdi_pick(const mdi_class_icon_t *table, size_t count, const char *device_class, bool is_on)
{
    for (size_t i = 0; i < count; i++) {
        if (table[i].device_class == NULL ||
            (device_class != NULL && strcmp(table[i].device_class, device_class) == 0)) {
            return is_on ? table[i].on : table[i].off;
        }
    }
    return "";
}

#define MDI_PICK(table, dc, on) mdi_pick((table), sizeof(table) / sizeof((table)[0]), (dc), (on))

static const char *mdi_default_icon(const char *domain, const char *device_class, bool is_on)
{
    static const mdi_class_icon_t k_switch[] = {
        {"outlet", "power-socket-de", "power-socket-de"},
        {NULL, "toggle-switch-variant", "toggle-switch-variant-off"},
    };
    static const mdi_class_icon_t k_cover[] = {
        {"blind", "blinds-open", "blinds"},
        {"shade", "roller-shade", "roller-shade-closed"},
        {"shutter", "window-shutter-open", "window-shutter"},
        {"curtain", "curtains", "curtains-closed"},
        {"garage", "garage-open", "garage"},
        {"gate", "gate-open", "gate"},
        {"door", "door-open", "door-closed"},
        {"awning", "awning-outline", "awning-outline"},
        {NULL, "window-open", "window-closed"},
    };
    static const mdi_class_icon_t k_sensor[] = {
        {"temperature", "thermometer", "thermometer"},
        {"humidity", "water-percent", "water-percent"},
        {"moisture", "water-percent", "water-percent"},
        {"carbon_dioxide", "molecule-co2", "molecule-co2"},
        {"carbon_monoxide", "molecule-co", "molecule-co"},
        {"pressure", "gauge", "gauge"},
        {"atmospheric_pressure", "gauge", "gauge"},
        {"illuminance", "brightness-5", "brightness-5"},
        {"power", "flash", "flash"},
        {"energy", "lightning-bolt", "lightning-bolt"},
        {"voltage", "sine-wave", "sine-wave"},
        {"current", "current-ac", "current-ac"},
        {"battery", "battery", "battery"},
        {"pm25", "air-filter", "air-filter"},
        {"pm10", "air-filter", "air-filter"},
        {"volatile_organic_compounds", "air-filter", "air-filter"},
        {"gas", "meter-gas", "meter-gas"},
        {"water", "water", "water"},
        {"signal_strength", "wifi", "wifi"},
        {"wind_speed", "weather-windy", "weather-windy"},
        {"precipitation", "weather-rainy", "weather-rainy"},
        {"timestamp", "clock-outline", "clock-outline"},
        {"monetary", "cash", "cash"},
        {NULL, "eye", "eye"},
    };
    static const mdi_class_icon_t k_binary[] = {
        {"door", "door-open", "door-closed"},
        {"garage_door", "garage-open", "garage"},
        {"window", "window-open", "window-closed"},
        {"opening", "square-outline", "square"},
        {"motion", "motion-sensor", "motion-sensor-off"},
        {"occupancy", "home", "home-outline"},
        {"presence", "home", "home-outline"},
        {"moisture", "water-alert", "water-off"},
        {"smoke", "smoke-detector-alert", "smoke-detector"},
        {"gas", "alert-circle", "check-circle"},
        {"battery", "battery-alert", "battery"},
        {"plug", "power-plug", "power-plug-off"},
        {"power", "power-plug", "power-plug-off"},
        {"light", "brightness-7", "brightness-5"},
        {"lock", "lock-open", "lock"},
        {"connectivity", "check-network-outline", "close-network-outline"},
        {NULL, "checkbox-marked-circle", "circle-outline"},
    };

    if (domain == NULL) {
        return "";
    }
    if (strcmp(domain, "switch") == 0) {
        return MDI_PICK(k_switch, device_class, is_on);
    }
    if (strcmp(domain, "light") == 0) {
        return is_on ? "lightbulb" : "lightbulb-outline";
    }
    if (strcmp(domain, "fan") == 0) {
        return is_on ? "fan" : "fan-off";
    }
    if (strcmp(domain, "cover") == 0) {
        return MDI_PICK(k_cover, device_class, is_on);
    }
    if (strcmp(domain, "sensor") == 0) {
        return MDI_PICK(k_sensor, device_class, is_on);
    }
    if (strcmp(domain, "binary_sensor") == 0) {
        return MDI_PICK(k_binary, device_class, is_on);
    }
    if (strcmp(domain, "button") == 0 || strcmp(domain, "input_button") == 0) {
        return "gesture-tap-button";
    }
    if (strcmp(domain, "script") == 0) {
        return "script-text";
    }
    if (strcmp(domain, "scene") == 0) {
        return "palette";
    }
    if (strcmp(domain, "automation") == 0) {
        return is_on ? "robot" : "robot-off";
    }
    if (strcmp(domain, "input_boolean") == 0) {
        return is_on ? "check-circle-outline" : "close-circle-outline";
    }
    if (strcmp(domain, "lock") == 0) {
        return is_on ? "lock-open" : "lock";
    }
    if (strcmp(domain, "climate") == 0) {
        return "thermostat";
    }
    if (strcmp(domain, "media_player") == 0) {
        return is_on ? "speaker-play" : "speaker-off";
    }
    if (strcmp(domain, "vacuum") == 0) {
        return "robot-vacuum";
    }
    if (strcmp(domain, "person") == 0 || strcmp(domain, "device_tracker") == 0) {
        return "account";
    }
    if (strcmp(domain, "siren") == 0) {
        return "bullhorn";
    }
    if (strcmp(domain, "valve") == 0) {
        return is_on ? "valve-open" : "valve-closed";
    }
    if (strcmp(domain, "water_heater") == 0) {
        return "water-boiler";
    }
    return "";
}

const char *ui_mdi_icon_for_entity(const char *tile_icon, const char *entity_id, bool is_on)
{
    if (tile_icon != NULL && tile_icon[0] != '\0' && ui_mdi_icon_codepoint(tile_icon) != 0) {
        return tile_icon;
    }
    if (entity_id == NULL || entity_id[0] == '\0') {
        return "";
    }

    /* The entity info is copied out of the model, so the result has to live
     * somewhere: icons are short static names except the HA icon, kept here. */
    static char s_ha_icon[APP_MAX_ICON_LEN];
    ha_entity_info_t info;
    const bool known = ha_model_get_entity(entity_id, &info);
    if (known && info.icon[0] != '\0' && ui_mdi_icon_codepoint(info.icon) != 0) {
        snprintf(s_ha_icon, sizeof(s_ha_icon), "%s", info.icon);
        return s_ha_icon;
    }

    char domain[32] = {0};
    const char *dot = strchr(entity_id, '.');
    if (dot != NULL && (size_t)(dot - entity_id) < sizeof(domain)) {
        memcpy(domain, entity_id, (size_t)(dot - entity_id));
    }
    return mdi_default_icon(domain, known ? info.device_class : NULL, is_on);
}

static bool mdi_contains_nocase(const char *haystack, const char *needle, size_t nlen)
{
    for (; *haystack != '\0'; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

size_t ui_mdi_icon_search(const char *query, const char **out, size_t max_out)
{
    if (out == NULL || max_out == 0) {
        return 0;
    }
    if (query != NULL && strncmp(query, "mdi:", 4) == 0) {
        query += 4;
    }
    const size_t qlen = (query != NULL) ? strlen(query) : 0;
    size_t written = 0;
    /* Prefix matches first, then names that contain the query elsewhere. */
    for (int pass = 0; pass < 2 && written < max_out; pass++) {
        for (size_t i = 0; i < g_mdi_names_count && written < max_out; i++) {
            const char *name = g_mdi_names[i].name;
            const bool prefix = (qlen == 0) || strncasecmp(name, query, qlen) == 0;
            if (pass == 0 ? !prefix : (prefix || !mdi_contains_nocase(name, query, qlen))) {
                continue;
            }
            out[written++] = name;
        }
        if (qlen == 0) {
            break;
        }
    }
    return written;
}
