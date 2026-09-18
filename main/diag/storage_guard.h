/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Guard window for deliberately long storage operations (microSD format,
 * firmware upload over HTTP).
 *
 * Both operations keep the CPU busy for tens of seconds: the flash write path
 * on ESP32-P4 parks the other core in the IPC wait, and the HTTP server task
 * never blocks on I/O, so the idle tasks simply do not run for that long.  Two
 * watchdogs then see "the system is stuck" from the outside:
 *
 *   - the IDF task watchdog watches the idle tasks
 *     (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU*) and resets the chip when one
 *     of them is starved for twice CONFIG_ESP_TASK_WDT_TIMEOUT_S (20 s with the
 *     default 10 s), reporting reset reason WDT(7).  This is what killed the
 *     panel in the middle of a firmware upload;
 *   - the panel's own UI stall watchdog (see diag/system_log.c) restarts the
 *     panel when the UI heartbeat stands still for 60 s, which is what killed
 *     it in the middle of a card format.
 *
 * Neither is an application bug, so while a window is open the idle tasks are
 * unsubscribed from the task watchdog (IDF stops the hardware timer once
 * nothing is left to watch) and the UI watchdog is paused.  Everything is
 * restored by the last storage_guard_end().  Windows nest and are counted, so
 * a background export that grabs the guard while a format is running cannot
 * close the format's window early.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens a window, or widens the one already open.  `reason` is a short literal
 * ("sd-format", "ota-upload", ...) kept for the duration log. */
void storage_guard_begin(const char *reason);

/* Closes the innermost window; the last one restores both watchdogs. */
void storage_guard_end(void);

/* True while any window is open.  Background tasks that also touch the storage
 * (log export, wallpaper cache) use this to stay out of the way. */
bool storage_guard_active(void);

/* Short window in which *no* background task may write to flash.
 *
 * esp_ota_end() validates the freshly written image with esp_image_verify(),
 * which maps every segment through esp_partition_mmap()/munmap() - i.e. it
 * reprograms MMU pages, each time parking the other core with the cache off.
 * This build executes .text/.rodata from PSRAM (CONFIG_SPIRAM_XIP_FROM_PSRAM=y)
 * through that very same MMU, so a flash write landing inside the remap window
 * makes the far core fetch through a page that is momentarily being
 * reprogrammed.  The result is rst:0x7 (HP_SYS_HP_WDT_RESET) about 1.6 s later
 * with no panic and no backtrace, i.e. every network OTA died in
 * esp_ota_end() while the persistent-log writer appended to LittleFS.
 *
 * The writers that matter (diag/system_log.c, ui/widgets/w_graph.c) hold their
 * bytes in RAM while paused and flush them once the window closes; the pause is
 * counted and lasts a few hundred milliseconds at most.  See
 * docs/WAVESHARE-7B-PORT.md §6.14. */
void storage_guard_flash_writers_pause(void);
void storage_guard_flash_writers_resume(void);
bool storage_guard_flash_writers_paused(void);

/* ---- Meter for the flash operations that can stall the DSI scanout --------
 *
 * Every write to the internal flash runs through
 * spi_flash_disable_interrupts_caches_and_other_cpu(): the caches are switched
 * off and *both* cores are parked until the erase/program finished.  The MIPI
 * DSI scanout, however, is re-armed from an interrupt once per frame (see
 * esp_lcd_panel_dpi.c, mipi_dsi_dma_trans_done_cb()), so a flash operation that
 * covers the end of a frame leaves the bridge FIFO dry and the panel shows the
 * bridge filler colour for as long as the operation lasts - the full-screen
 * flash this panel is being debugged for.
 *
 * The meter below is what makes that attribution measurable instead of
 * theoretical: diag/dsi_underrun_watch.c probes it from the frame interrupt,
 * so a reported screen gap names the flash operation that was in flight.
 * The durations double as a duty cycle (total_ms over the uptime): the share
 * of the time the display is left to the filler is roughly
 * total_ms / frame_period.
 *
 * Kinds are for attribution only; pass the one that fits the caller. */
typedef enum {
    FLASH_OP_LOG = 0,  /* persistent log append                          */
    FLASH_OP_GRAPH,    /* widget history rewrite                         */
    FLASH_OP_NVS,      /* settings commit                                */
    FLASH_OP_UI,       /* uploaded image / layout / i18n write           */
    FLASH_OP_KIND_COUNT
} flash_op_kind_t;

void storage_guard_flash_op_begin(flash_op_kind_t kind);
void storage_guard_flash_op_end(flash_op_kind_t kind);

/* True while an operation sits inside the flash driver, or when one ended
 * within `within_ms` before `now_ms`; `out_kind`/`out_ms` then carry that
 * operation's kind and duration (in flight: the time already spent in it).
 * Written for interrupt context: no locks, only volatile loads. */
bool storage_guard_flash_op_probe(uint32_t now_ms, uint32_t within_ms,
                                  uint32_t *out_kind, uint32_t *out_ms);

uint32_t storage_guard_flash_op_count(uint32_t kind);
uint32_t storage_guard_flash_op_total_ms(uint32_t kind);
uint32_t storage_guard_flash_op_max_ms(uint32_t kind);
uint32_t storage_guard_flash_op_slow(uint32_t kind);
const char *storage_guard_flash_op_kind_name(uint32_t kind);

#ifdef __cplusplus
}
#endif
