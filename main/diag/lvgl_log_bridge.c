/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "diag/lvgl_log_bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "lvgl.h"

#include "diag/system_log.h"
#include "util/log_tags.h"

/* The callback signature only exists when LVGL logging is compiled in, which is
 * true for the panel7 build (CONFIG_LV_USE_LOG=y).  Every other variant keeps
 * the no-op stubs below, so this file is safe to compile everywhere. */
#if defined(LV_USE_LOG) && LV_USE_LOG

/* LVGL can emit the same complaint once per frame; more than this many
 * identical consecutive lines are collapsed into a "(xN repeats)" summary so
 * one broken style call cannot bury the rest of the log. */
#define LVGL_LOG_REPEAT_BURST 5U
#define LVGL_LOG_SIG_LEN      192U

static char     s_last_sig[LVGL_LOG_SIG_LEN];
static unsigned s_last_count;
static unsigned s_suppressed_total;
static bool     s_burst_open;

static unsigned lvgl_log_sig_offset(const char *buf)
{
    /* Strip the timestamp LVGL prepends when LV_LOG_USE_TIMESTAMP is on, so
     * "same message at a different ms" still counts as a repeat. */
    if (buf == NULL) {
        return 0;
    }
    if (buf[0] == '[') {
        const char *close = strchr(buf, ']');
        if (close != NULL && close[1] == ' ') {
            return (unsigned)(close + 2 - buf);
        }
    }
    return 0;
}

static void lvgl_log_bridge_flush_burst(void)
{
    if (s_burst_open) {
        s_burst_open = false;
        if (s_last_count > LVGL_LOG_REPEAT_BURST) {
            system_log_write_info(TAG_LVGL, "previous LVGL line repeated %u times",
                                  s_last_count);
        }
    }
    s_last_count = 0;
    s_last_sig[0] = '\0';
}

static void lvgl_log_print_cb(lv_log_level_t level, const char *buf)
{
    if (buf == NULL) {
        return;
    }

    const char *msg = buf + lvgl_log_sig_offset(buf);

    /* Repeat detector on the message part only. */
    bool repeat = (s_last_sig[0] != '\0') && (strncmp(msg, s_last_sig, sizeof(s_last_sig) - 1) == 0);
    if (repeat) {
        s_last_count++;
        if (s_last_count > LVGL_LOG_REPEAT_BURST) {
            s_suppressed_total++;
            s_burst_open = true;
            return;
        }
    } else {
        lvgl_log_bridge_flush_burst();
        snprintf(s_last_sig, sizeof(s_last_sig), "%s", msg);
        s_last_count = 1;
    }

    esp_log_level_t esp_level = ESP_LOG_WARN;
    if (level == LV_LOG_LEVEL_ERROR) {
        esp_level = ESP_LOG_ERROR;
    } else if (level == LV_LOG_LEVEL_INFO || level == LV_LOG_LEVEL_TRACE) {
        esp_level = ESP_LOG_INFO;
    }

    /* Goes to the console and, through the capture hook, into the persistent
     * log ring.  Nothing here calls back into LVGL, so there is no recursion. */
    esp_log_write(esp_level, TAG_LVGL, "%s", buf);
}

void lvgl_log_bridge_init(void)
{
    lv_log_register_print_cb(lvgl_log_print_cb);
    system_log_write_info(TAG_LVGL, "LVGL log callback registered (compiled level %d)",
                          (int)LV_LOG_LEVEL);
}

void lvgl_log_bridge_note(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    system_log_write_info(TAG_LVGL, "%s", line);
}

void lvgl_log_bridge_stats(unsigned *seen, unsigned *suppressed)
{
    if (seen != NULL) {
        *seen = 0;
    }
    if (suppressed != NULL) {
        *suppressed = s_suppressed_total;
    }
}

#else /* LV_USE_LOG */

void lvgl_log_bridge_init(void)
{
}

void lvgl_log_bridge_note(const char *fmt, ...)
{
    (void)fmt;
}

void lvgl_log_bridge_stats(unsigned *seen, unsigned *suppressed)
{
    if (seen != NULL) {
        *seen = 0;
    }
    if (suppressed != NULL) {
        *suppressed = 0;
    }
}

#endif /* LV_USE_LOG */
