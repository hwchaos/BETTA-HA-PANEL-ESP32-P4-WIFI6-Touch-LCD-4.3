/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct {
    size_t used_bytes;    /* Bytes currently held by LVGL */
    size_t peak_bytes;    /* Highest used_bytes since boot or last reset */
    uint32_t block_count; /* Live LVGL allocations */
    uint32_t alloc_count; /* Successful allocations since boot */
    uint32_t free_count;  /* Frees since boot */
    uint32_t fail_count;  /* Allocations that returned NULL */
} ui_lvgl_mem_stats_t;

/* Fills `out` with the allocator counters (zeroed when the custom allocator is
 * compiled out). NULL is ignored. */
void ui_lvgl_mem_get_stats(ui_lvgl_mem_stats_t *out);

/* Drops the peak watermark down to the current usage. */
void ui_lvgl_mem_reset_peak(void);
