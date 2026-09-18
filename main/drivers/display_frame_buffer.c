/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "drivers/display_init.h"

#include <stddef.h>

/* Weak fallbacks for panel variants that do not hand out their scan-out
 * framebuffers.  The 7" driver (display_init_panel7.c) provides strong
 * definitions; callers must treat NULL/"0 buffers" as "capture not available"
 * and use another path. */
__attribute__((weak)) int display_frame_buffer_count(void)
{
    return 0;
}

__attribute__((weak)) void *display_frame_buffer_get(int index, size_t *out_bytes)
{
    (void)index;
    if (out_bytes != NULL) {
        *out_bytes = 0;
    }
    return NULL;
}

/* Panel variants without render instrumentation report "nothing measured" so
 * the heartbeat prints zeros instead of failing to link. */
__attribute__((weak)) void display_render_stats_get(display_render_stats_t *out)
{
    if (out != NULL) {
        *out = (display_render_stats_t){0};
    }
}

__attribute__((weak)) void display_render_stats_format(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
}

__attribute__((weak)) bool display_render_note_take(display_render_note_t *out)
{
    (void)out;
    return false;
}

/* Panel variants without a backlight ramp: apply the target immediately, so the
 * screensaver's fade degrades to the plain step there. */
__attribute__((weak)) esp_err_t display_fade_brightness_percent(int percent, uint32_t duration_ms)
{
    (void)duration_ms;
    return display_set_brightness_percent(percent);
}

/* Panel variants that do not distinguish the wake source: drop it and behave
 * exactly like display_note_activity(). The 7" driver overrides this with a
 * version that also logs what woke the panel. */
__attribute__((weak)) void display_note_activity_from(const char *source)
{
    (void)source;
    display_note_activity();
}
