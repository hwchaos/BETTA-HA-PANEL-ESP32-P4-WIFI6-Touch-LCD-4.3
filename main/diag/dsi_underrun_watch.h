/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * MIPI-DSI bridge underrun detector: the missing half of "why did the screen
 * flash light blue?".
 *
 * display_flash_watch.c only watches the *content* of the scan-out
 * framebuffers, so it can only see a flash somebody drew.  The light-blue
 * flash seen while using the panel is not drawn - it is generated inside the
 * DSI bridge when it cannot fetch pixels from PSRAM fast enough.  Espressif's
 * own driver documents it (components/esp_lcd/dsi/esp_lcd_panel_dpi.c):
 *
 *   // when an underrun happens, the LCD display may already becomes blue ...
 *   ESP_DRAM_LOGE(TAG, "can't fetch data from external memory fast enough ...");
 *
 * That message is written with ESP_DRAM_LOGE, i.e. straight to the ROM
 * console: it never reaches this firmware's log ring, the heartbeat or the
 * SD-card log.  That is exactly why the panel could flash without a single log
 * line.  This module takes over that reporting.
 *
 * Mechanism:
 *   - The underrun itself is invisible to this firmware, but *what it looks
 *     like* is not: the bridge substitutes dpi_rsv_data (reset value 16383 =
 *     bright cyan) for every pixel it could not fetch, and
 *     esp_lcd_panel_dpi.c sets fifo_underrun_discard_vcnt to the panel width,
 *     i.e. 1024 lines - more than a 600-line frame has.  That field is the
 *     underrun *interrupt mask* ("when underrun occurs and line cnt is less
 *     than this field"), so with 1024 > 600 the sticky underrun bit never
 *     latches at all.  start() rewrites both registers - discard_vcnt = 0 so
 *     nothing is masked, black reserved data so the substitution paints a dark
 *     blink instead of a bright cyan one - and calls
 *     mipi_dsi_brg_ll_update_dpi_config(), because both registers are
 *     double-buffered and are dead stores without that write-1-to-apply bit.
 *   - The IDF underrun interrupt is disabled.  Its only effect was that
 *     invisible ROM print, and turning it off leaves the sticky `int_raw`
 *     underrun bit (R/WTC/SS) set until software clears it, so no event can
 *     ever be missed even if we look at it late.
 *   - A 1 ms task latches that bit, counts the event, timestamps it and groups
 *     events that arrive close together into bursts.
 *   - The bridge DMA FIFO depth is sampled by the same task.  It is the
 *     leading indicator: the closer it gets to 0, the closer the next underrun
 *     is, so `fifo_min=` is what proves whether a fix helped even when the
 *     underrun count itself is too low to be conclusive.  `fifo_low=` counts
 *     the 1 ms samples that were below the DMA refill threshold, which shows
 *     how marginal the margin is even when nothing has underrun yet.
 *   - Every bandwidth-heavy consumer brackets itself with
 *     dsi_bus_activity_begin()/end(), so an underrun is attributed to whatever
 *     was running at that moment: "dsi: underrun busy=cam+sd ...".  If a source
 *     is never present while the panel flashes, that source is not the cause.
 *
 * Extra witnesses, for the case where the count is still lower than what the
 * user sees:
 *   - The bridge has no free-running starvation counter, so the FIFO depth is
 *     sampled back-to-back for DSIW_FIFO_BURST_US every millisecond (~20 % duty
 *     cycle).  `fbmin=`, `f0=`/`fe=` and `fz=` catch a dry FIFO even when the
 *     interrupt latch never fires, and each such episode is logged with
 *     everything else that was running at that moment.
 *   - The DSI host's live PHY status is sampled on every pass.  `unlk=` counts
 *     the D-PHY losing lock, `lane=` counts ULPS / stop-state transitions and
 *     `psx=` records which lane states were ever seen; the host's masked
 *     `int_st0`/`int_st1` bits (D-PHY errors, ECC/CRC/packet errors and
 *     `dpi_buff_pld_under`) are latched into `st0=`/`st1=` and each new bit is
 *     logged once.  A lost link leaves the glass undriven, and an undriven TFT
 *     shows its backlight - a light screen, self-recovering - which is a
 *     content-free flash that no framebuffer and no backlight reading can
 *     explain.
 *   - The host's own pixel payload FIFO (`vid_pkt_status.dpi_buff_pld_empty`) is
 *     sampled too (`hpz=`/`hpr=`): when the DWC host has no pixel to send it
 *     pads the line with null packets, which is a second, independent way to
 *     get a light screen with a perfectly fed bridge.
 *   - The two mitigation registers are read back every 250 ms and re-asserted
 *     if anything rewrote them (`clob=`), so the flash can never come back
 *     because some other code quietly restored IDF's defaults.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Consumers whose PSRAM traffic can starve the DSI bridge.  Keep the order
 * stable - it is the order used in the summary line. */
typedef enum {
    DSI_BUS_CAMERA = 0, /* camera frame copy / CSI capture          */
    DSI_BUS_SD,         /* SD card log export / file IO             */
    DSI_BUS_SCREENSHOT, /* /api/screenshot.bmp streaming            */
    DSI_BUS_JPEG,       /* JPEG encode of a camera frame            */
    DSI_BUS_WALLPAPER,  /* screensaver wallpaper load / decode      */
    DSI_BUS_RENDER,     /* LVGL full-screen repaint (DMA2D blits)   */
    DSI_BUS_SRC_COUNT,
} dsi_bus_src_t;

/* Start the detector.  No-op on panel variants without the DSI bridge.
 * Call once after display_init() - re-entrant calls are ignored. */
esp_err_t dsi_underrun_watch_start(void);

/* Mark the start/end of a heavy PSRAM burst.  Cheap, ISR/task safe and
 * nestable; every begin() must be paired with an end(). */
void dsi_bus_activity_begin(dsi_bus_src_t src);
void dsi_bus_activity_end(dsi_bus_src_t src);

/* True while at least one consumer of `src` is inside its burst. */
bool dsi_bus_activity_active(dsi_bus_src_t src);

/* One-line snapshot for the heartbeat / diagnostics:
 *   dsi_underruns=3 bursts=2 worst=2 last=45s fifo_min=17 fifo_last=231
 *   corr[cam=2 sd=0 shot=1 jpeg=0 wp=0 rnd=0] busy=1 poll=1ms disc=0 rsv=0
 *   host[unlk=0 since=0s unl=0 lane=0 psx=0x00 st0=0x00000000 st1=0x00000000 ste=0]
 *   f0=0 fe=0 fbmin=349 clob=0 fz=0 hpz=0 hpr=0 hpmax=1ms hpl=0 msk1=0x00000000
 * `rsv` is read back from the register: it shows which colour the bridge paints
 * on a starvation, i.e. which colour a filler flash has. */
int dsi_underrun_watch_summary(char *buf, size_t len);

/* Total underruns since boot (hardware event, never saturated). */
uint32_t dsi_underrun_watch_count(void);

/* Total number of underrun bursts (groups separated by >200 ms of silence). */
uint32_t dsi_underrun_watch_bursts(void);

/* Milliseconds since the last underrun, or UINT32_MAX if none yet. */
uint32_t dsi_underrun_watch_since_last_ms(void);

/* Lowest DSI bridge FIFO depth seen since start(), or UINT32_MAX if unknown. */
uint32_t dsi_underrun_watch_fifo_min(void);

/* Burst FIFO samples that read a depth of exactly 0 - a starvation witness
 * that does not depend on the interrupt latch ever firing. */
uint32_t dsi_underrun_watch_fifo_zero(void);

/* Gap-separated episodes of those zero-depth samples (one per starvation dip,
 * not one per sample). */
uint32_t dsi_underrun_watch_fifo_zero_runs(void);

/* Times the DWC host's pixel payload FIFO (vid_pkt_status.dpi_buff_pld_empty)
 * was found dry.  Non-zero means the host had no pixel to send and padded the
 * line with null packets - a light screen with a perfectly fed bridge.  Most of
 * these are the normal gap between two lines, so the two counters below are the
 * ones that mean something: how long the longest dry streak was, and how many
 * were long enough to cover real lines. */
uint32_t dsi_underrun_watch_host_pld_empty(void);

/* Longest continuous "host had no pixel to send" streak, in milliseconds. */
uint32_t dsi_underrun_watch_host_pld_max_ms(void);

/* Dry streaks lasting at least DSIW_HOST_PLD_LONG_MS - each one is a stretch of
 * scan-out lines the panel received as null packets. */
uint32_t dsi_underrun_watch_host_pld_long_runs(void);

/* How often a DSI host status bit went 0 -> 1 (D-PHY errors, packet errors,
 * dpi_buff_pld_under).  Counted as edges, not as levels, because several of
 * those bits are sticky and would hide every event after the first. */
uint32_t dsi_underrun_watch_host_status_edges(void);

/* Times the D-PHY dropped lock (phy_lock 1 -> 0).  A non-zero value means the
 * glass was left undriven at least once, which is exactly what a light flash
 * with an unchanged framebuffer and an unchanged backlight looks like. */
uint32_t dsi_underrun_watch_phy_unlocks(void);

/* Times the underrun artifact mitigation had to be re-applied because the two
 * registers had been rewritten behind us. */
uint32_t dsi_underrun_watch_clobbers(void);

/* ---- Frame-gap witness ---------------------------------------------------
 * The scan-out DMA is re-armed by software from an interrupt once per frame
 * (esp_lcd_panel_dpi.c), so the interval between two frame interrupts is the
 * frame period - unless a frame boundary fell inside a window with the
 * interrupts masked (every write to the internal flash opens one).  Then the
 * DMA was not re-armed, the bridge FIFO ran dry and the panel showed the
 * bridge's filler colour until the next interrupt, which arrives late by
 * exactly the stall duration.
 *
 * One late interrupt therefore equals one visible screen flash, which is what
 * makes this the instrument that can *prove* the fix: compare `gaps` across a
 * change.  It is the only witness that survives a flash freeze, because it runs
 * in the interrupt that reports the event.
 *
 * Unlike the panel's own callback slots (one of which the LVGL adapter
 * overwrites right after the panel is created), this is armed automatically
 * when IDF registers the scan-out DMA channel - nothing has to call it. */
typedef struct {
    bool armed;          /* the scan-out DMA registration was intercepted    */
    uint32_t frames;      /* frame interrupts since boot (~60/s when healthy) */
    uint32_t gaps;        /* stalls long enough to show: one per screen flash */
    uint32_t max_interval_us; /* worst interval of *any* frame, in us; the
                               * frame period is ~16300 us, so every us above
                               * that is glass left to the bridge filler      */
    uint32_t max_ms;      /* longest stall                                    */
    uint32_t last_ms;     /* most recent stall                                */
    uint32_t with_flash;  /* gaps with a flash write in flight or just ended  */
    uint32_t lost;        /* gaps that arrived faster than they could be kept */
} dsi_frame_gap_stats_t;

/* Snapshot of the frame-gap counters, for the diagnostics API.
 * No-op on panel variants without the DSI bridge. */
void dsi_frame_gap_watch_stats(dsi_frame_gap_stats_t *out);

#ifdef __cplusplus
}
#endif
