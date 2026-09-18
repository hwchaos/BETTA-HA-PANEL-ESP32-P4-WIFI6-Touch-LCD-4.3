/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * See dsi_underrun_watch.h for why this exists.  Short version: the DSI bridge
 * pulls pixels straight out of PSRAM and, if it cannot keep its DMA FIFO fed,
 * it stops sending real pixels and substitutes dpi_rsv_data - reset value 16383,
 * i.e. bright cyan - for as long as the starvation lasts.  That substitution is
 * what the user sees as a light flash.  The event is a latchable interrupt
 * status bit, so this module can count it exactly - and, by bracketing the heavy
 * PSRAM consumers, say who was hogging the bus when it happened.
 */

#include "diag/dsi_underrun_watch.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "diag/display_flash_watch.h"
#include "diag/storage_guard.h"
#include "diag/system_log.h"

#define DSIW_FIFO_UNKNOWN 0xFFFFFFFFu

#if defined(CONFIG_APP_PANEL_VARIANT_7INCH_1024) && defined(CONFIG_IDF_TARGET_ESP32P4)

#include "esp_attr.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_private/dw_gdma.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "hal/mipi_dsi_host_ll.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "soc/mipi_dsi_host_struct.h"
#include "ui/ui_pages.h"

static const char *TAG = "dsiwatch";

/* MIPI_DSI_BRG_LL_EVENT_UNDERRUN, bit 0 of int_ena/int_raw/int_clr/int_st */
#define DSIW_UNDERRUN_BIT    (1U << 0)
#define DSIW_POLL_MS         1     /* int_raw latches, so this only sets the
                                    * timestamp resolution, never the count   */
#define DSIW_EPISODE_GAP_MS  200   /* silence that separates two bursts        */
#define DSIW_LOG_INTERVAL_MS 1000  /* never flood the log in a sustained burst */
#define DSIW_TASK_STACK      8192  /* 4 KB was too small: the log path below
                                    * nests three 640-byte line buffers, and the
                                    * task took itself out with a stack overflow
                                    * (pc=0x20000000) after roughly 12 minutes of
                                    * uptime, rebooting the panel. */
#define DSIW_TASK_PRIO       3

/* What the panel shows when the bridge FIFO runs dry.  Two registers decide it,
 * and IDF leaves both in a state where a single starvation event paints bright
 * cyan over the whole screen - which is exactly the light blue flash the user
 * sees on the menu and on the screensaver:
 *
 *  - dpi_rsv_data (dsi_brg_dpi_rsv_dpi_data_reg_t) is "the pixel data sent to
 *    dsi_host when dsi_bridge fifo underflow".  Its reset value is 16383 =
 *    RGB565 0x3FFF = cyan at full brightness, and nothing in IDF ever writes it.
 *  - fifo_underrun_discard_vcnt (dsi_brg_dpi_misc_config_reg_t) is documented
 *    as "this field configures the underrun interrupt musk [mask], when
 *    underrun occurs and line cnt is less then this field".  It is a *mask*,
 *    not a window of lines that get replaced: an underrun raised while the
 *    frame is inside the first `vcnt` lines does not reach int_raw.bit0 at all,
 *    and the substitution for those lines is dropped with it.
 *    esp_lcd_panel_dpi.c sets it to
 *    mipi_dsi_brg_ll_set_underrun_discard_count(video_timing.h_size) = 1024,
 *    and a frame is only 600 lines long - so line_cnt is *always* below it,
 *    every underrun is inside the mask, and the bit never latches.  That is why
 *    this watcher counted 0 underruns for as long as the panel flashed, and why
 *    the flash stayed light blue.
 *
 * The previous value here (8 lines) was still wrong in the same way: the
 * starvation that produces the visible flash begins at the frame boundary
 * (line_cnt 0 - the DW-GDMA link list is exhausted at the end of every frame
 * and only the transfer-done ISR restarts it), i.e. inside the first few lines
 * and therefore inside the mask - invisible to the latch and excluded from the
 * substitution, which is why the count stayed 0 and a cyan-looking flash
 * remained possible.
 *
 * So: discard_vcnt = 0 (nothing masked - the latch finally sees every event,
 * including the frame-boundary one) and reserved data black (a starvation event
 * becomes a barely visible dark blink instead of a white-blue strobe; 0 is
 * black in every colour format, so this cannot misfire).
 *
 * Both registers are double-buffered: they only take effect after
 * dpi_config_update is written, which is what mipi_dsi_brg_ll_update_dpi_config()
 * below does - without it the writes above are dead stores into a shadow. */
#define DSIW_DISCARD_LINES   0u
/* --- STATE (measured, supersedes the reading above) ----------------------
 * With discard_vcnt = 0 the underrun latch stayed at 0 for ten minutes of
 * uptime while fifo_min never left the middle of the FIFO (509 of 1024 words,
 * in the 1 ms polls and in the 200 us bursts), i.e. the bridge was fed
 * continuously the whole time.  So either the mask reading above is wrong, or
 * the starvation the user sees happens downstream of the bridge.  Both stay
 * open until the colour below has been watched once, so bit 0 keeps counting
 * (it costs nothing) and the filler becomes a marker instead of an argument.
 * ------------------------------------------------------------------------ */
/* The colour of a bridge-filler flash - the discriminator between the two
 * remaining mechanisms, and it needs no tooling: the user watches the panel.
 *   magenta flash  -> the flash IS the bridge filler (bandwidth/starvation);
 *   still light blue -> the filler is innocent: the glass is being left
 *   undriven by the link/panel instead, and nothing is being drawn (the
 *   framebuffer and the backlight never move - proven by display_flash_watch).
 * The magenta probe answered that question (the filler is the artifact), so the
 * filler is back to black: if any starvation survives, the panel blinks dark
 * instead of flooding white-blue, and a *bright* flash would falsify the whole
 * explanation.  The scanout stall itself is measured by the frame-gap witness
 * below, which is what the fix has to drive to zero. */
#define DSIW_RSV_PROBE       0x0000u
/* raw_buf_almost_empty_thrd, the level the DMA is asked to start refilling at
 * (esp_lcd_panel_dpi.c: 1024 - 256).  The FIFO dipping below it is the earliest
 * symptom of the starvation that ends in an underrun. */
#define DSIW_FIFO_LOW        768u

/* --- Witnesses for a flash that is not framebuffer content ----------------
 * display_flash_watch.c proved that the light-blue flash is not drawn: the
 * scan-out buffers never brighten, the backlight duty never moves, and this
 * module's underrun counter stays at zero.  That leaves exactly two places the
 * flash can come from, and both are observable from here:
 *
 *  1. the bridge briefly stops feeding the host - a starvation dip too short
 *     for the interrupt latch, or one the discard window hides;
 *  2. the DSI host/PHY link hiccups (lock loss, ULPS/stop-state excursion, a
 *     D-PHY error), which leaves the glass undriven, and an undriven TFT shows
 *     its backlight - a light screen - until the link recovers by itself.
 *
 * The bridge has no free-running underrun counter, so (1) is caught by reading
 * the FIFO depth in a dense burst instead of once per millisecond, and (2) by
 * sampling the host's live PHY status.  The host is read ONLY: it has no
 * int_raw/int_clr (just masked status plus mask/force), so clearing its bits
 * would mean poking IDF's mask registers and breaking its DCS handling. */
#define DSIW_FIFO_BURST_US  200u  /* dense FIFO sampling window per poll pass  */
#define DSIW_FIFO_BURST_MAX 4096u /* hard cap so a slow register cannot hang us*/
#define DSIW_FIFO_EDGE     64    /* "the DMA is about to run dry" level       */
#define DSIW_WD_PERIOD_MS  250   /* read the mitigation back this often       */
#define DSIW_PHY_LOG_MS    1000  /* rate limit for PHY/link event lines       */
#define DSIW_HOST_PLD_EMPTY_BIT (1u << 16) /* dsi_host_vid_pkt_status:
                                            * dpi_buff_pld_empty, the DWC host's
                                            * pixel payload FIFO is dry        */
/* The payload FIFO empties briefly between lines all the time (208 sampled dry
 * runs per second), so only a dry *streak* is news: at 636 lines and 60 frames
 * per second one line is 26 us, so 3 ms of "no pixel to send" is over a hundred
 * lines - most of a frame of null packets, which is a light screen. */
#define DSIW_HOST_PLD_LONG_MS 3u
#define DSIW_HOST_ST_LOG_MAX  8u   /* host status edges that still get a line   */

static const char *const k_src_name[DSI_BUS_SRC_COUNT] = {"cam", "sd", "shot", "jpeg", "wp", "rnd"};

/* The bridge's *registers* live at MIPI_DSI_BRIDGE (0x500A0800).  Do not use
 * MIPI_DSI_BRG_MEM_BASE here: on the P4 that symbol is MIPI_DSI_MEM
 * (0x50105000), the write-only pixel window the GDMA feeds, and *reading* it
 * raises a load access fault - which is exactly what a boot loop looks like. */
static dsi_brg_dev_t *const s_brg = MIPI_DSI_LL_GET_BRG(0);

/* The DSI host owns the PHY and the link state (MIPI_DSI_HOST, 0x500A0000). */
static dsi_host_dev_t *const s_host = MIPI_DSI_LL_GET_HOST(0);

/* Bus-activity bookkeeping: written by arbitrary tasks, read by the poll task */
static volatile uint32_t s_bus_depth[DSI_BUS_SRC_COUNT];
static uint32_t s_busy_start_ms[DSI_BUS_SRC_COUNT];

/* Counters owned by the poll task, read by the heartbeat */
static volatile uint32_t s_underruns;
static volatile uint32_t s_bursts;
static volatile uint32_t s_worst_burst;
static volatile uint32_t s_last_ms;
static volatile uint32_t s_corr[DSI_BUS_SRC_COUNT];
static volatile uint32_t s_busy_ms[DSI_BUS_SRC_COUNT];
static volatile uint32_t s_fifo_min = DSIW_FIFO_UNKNOWN;
static volatile uint32_t s_fifo_last = DSIW_FIFO_UNKNOWN;
static volatile uint32_t s_fifo_low;   /* 1 ms samples with fifo < DSIW_FIFO_LOW */
static volatile bool s_started;

/* Witness counters for the content-free flash (see DSIW_FIFO_BURST_US block). */
static volatile uint32_t s_fifo_zero;  /* burst samples that read exactly 0      */
static volatile uint32_t s_fifo_zero_runs; /* gap-separated zero-depth episodes  */
static volatile uint32_t s_fifo_edge;  /* burst samples <= DSIW_FIFO_EDGE        */
static volatile uint32_t s_host_pld_empty;      /* host payload FIFO dry samples */
static volatile uint32_t s_host_pld_empty_runs; /* how often it went dry         */
static volatile uint32_t s_host_pld_empty_ms;   /* when it was last dry          */
static volatile uint32_t s_host_pld_long_runs;  /* dry runs >= DSIW_HOST_PLD_LONG_MS */
static volatile uint32_t s_host_pld_max_ms;     /* longest continuous dry run    */
static volatile uint32_t s_host_st_edges;       /* host status bits going 0 -> 1 */
static volatile uint32_t s_fifo_burst_min = DSIW_FIFO_UNKNOWN;
static volatile uint32_t s_phy_unlocks;    /* phy_lock 1 -> 0 transitions        */
static volatile uint32_t s_phy_unlock_ms;  /* when the last one happened         */
static volatile uint32_t s_phy_unlocked;   /* samples with phy_lock == 0         */
static volatile uint32_t s_phy_lane_chg;   /* ULPS / stop-state transitions      */
static volatile uint32_t s_phy_state_or;   /* OR of every lane-state mask seen   */
static volatile uint32_t s_host_st0_or;    /* every host int_st0 bit ever seen   */
static volatile uint32_t s_host_st1_or;    /* every host int_st1 bit ever seen   */
static volatile uint32_t s_clobbers;       /* times the mitigation was rewritten */

static void underrun_artifact_mitigation(void);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void dsi_bus_activity_begin(dsi_bus_src_t src)
{
    if (src >= DSI_BUS_SRC_COUNT) {
        return;
    }
    uint32_t depth = __atomic_add_fetch(&s_bus_depth[src], 1u, __ATOMIC_SEQ_CST);
    if (depth == 1u) {
        /* first (outermost) begin: this is where the burst time starts */
        s_busy_start_ms[src] = now_ms();
    }
}

void dsi_bus_activity_end(dsi_bus_src_t src)
{
    if (src >= DSI_BUS_SRC_COUNT) {
        return;
    }
    uint32_t depth = __atomic_load_n(&s_bus_depth[src], __ATOMIC_SEQ_CST);
    if (depth == 0u) {
        return; /* unbalanced call: ignore rather than corrupt the depth */
    }
    if (depth == 1u) {
        if (s_started) {
            s_busy_ms[src] += now_ms() - s_busy_start_ms[src];
        }
    }
    __atomic_sub_fetch(&s_bus_depth[src], 1u, __ATOMIC_SEQ_CST);
}

bool dsi_bus_activity_active(dsi_bus_src_t src)
{
    if (src >= DSI_BUS_SRC_COUNT) {
        return false;
    }
    return __atomic_load_n(&s_bus_depth[src], __ATOMIC_SEQ_CST) != 0u;
}

/* "cam+sd", "none" */
static void busy_list(char *out, size_t len)
{
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < DSI_BUS_SRC_COUNT; i++) {
        if (__atomic_load_n(&s_bus_depth[i], __ATOMIC_SEQ_CST) == 0u) {
            continue;
        }
        if (used) {
            used += (size_t)snprintf(out + used, len - used, "+");
        }
        used += (size_t)snprintf(out + used, len - used, "%s", k_src_name[i]);
        if (used >= len) {
            return;
        }
    }
    if (out[0] == '\0') {
        snprintf(out, len, "none");
    }
}

static const char *host_bit_name(int reg, uint32_t bit)
{
    if (reg == 1) {
        switch (bit) {
        case 0: return "to_hs_tx";
        case 1: return "to_lp_rx";
        case 2: return "ecc_single_err";
        case 3: return "ecc_milti_err";
        case 4: return "crc_err";
        case 5: return "pkt_size_err";
        case 6: return "eopt_err";
        case 7: return "dpi_pld_wr_err";
        case 19: return "dpi_buff_pld_under";
        default: return "gen_cmd";
        }
    }
    if (bit <= 15u) {
        return "ack_with_err";
    }
    if (bit <= 20u) {
        return "dphy_error";
    }
    return "other";
}

/* Log a host status bit that just went 0 -> 1.  Both registers are *masked*
 * status (only the bits IDF enabled in int_msk0/1 show up), so a zero read is
 * "nothing enabled/latched", not proof that the link was perfect - the live
 * phy_lock sampling in poll_task() is what covers that case.  Some of these bits
 * are sticky (int_st0 bit 20 latched ~1 s after boot and never cleared again),
 * so logging only the first-ever sighting hid every later event: the caller
 * passes this pass's edges, and only the first DSIW_HOST_ST_LOG_MAX of them get
 * a line - after that the counters carry it, because a flapping status bit must
 * not own the log ring again. */
static void host_status_note(uint32_t st0, uint32_t st1, uint32_t edge0, uint32_t edge1, uint32_t phy,
                             uint32_t now)
{
    static uint32_t logged;
    char list[160];
    size_t used = 0;
    list[0] = '\0';
    for (int reg = 0; reg < 2; reg++) {
        const uint32_t v = reg ? edge1 : edge0;
        for (uint32_t bit = 0; bit < 32u; bit++) {
            if (!(v & (1u << bit))) {
                continue;
            }
            used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%d.%u(%s)", used ? "+" : "",
                                     reg, (unsigned)bit, host_bit_name(reg, bit));
            if (used >= sizeof(list)) {
                list[sizeof(list) - 1] = '\0';
                break;
            }
        }
    }

    if (logged >= DSIW_HOST_ST_LOG_MAX) {
        return;
    }
    logged++;

    system_log_event("dsiwatch", "dsi host status edge #%u: %s st0=0x%08x st1=0x%08x phy=0x%08x t=%ums",
                     (unsigned)logged, list, (unsigned)st0, (unsigned)st1, (unsigned)phy,
                     (unsigned)now);
    ESP_LOGW(TAG, "dsi host status edge #%u: %s (st0=0x%08x st1=0x%08x phy=0x%08x)", (unsigned)logged,
             list, (unsigned)st0, (unsigned)st1, (unsigned)phy);
    display_flash_watch_note("dsi:host");
}

/* --- Frame-gap witness ---------------------------------------------------
 * Every other witness in this file samples the hardware, so all of them go
 * blind exactly when it matters: a write to the internal flash switches the
 * caches off and parks *both* cores (spi_flash_disable_interrupts_caches_and_other_cpu),
 * and this firmware's own code runs from PSRAM (CONFIG_SPIRAM_XIP_FROM_PSRAM),
 * so nothing is executed while the write lasts.
 *
 * The one thing that keeps running is the hardware, and the hardware tells us
 * what happened: esp_lcd_panel_dpi.c re-arms the scan-out DMA from the DW-GDMA
 * "transfer done" interrupt once per frame (is_last = true).  The interval
 * between two of those interrupts is therefore the frame period - unless a
 * frame boundary fell inside a window in which interrupts were masked.  Then
 * the DMA was not re-armed, the bridge FIFO drained its ~20 us of runway and
 * the panel showed the filler colour for the whole stall, so the *next*
 * interrupt arrives late by roughly the stall duration.
 *
 * That makes the gap the objective artifact meter: one late interrupt = one
 * visible flash = one entry here.  Counting them across a change is how the fix
 * is proved rather than eyeballed, and probing the flash-operation meter from
 * inside the interrupt names the operation that was in flight.
 *
 * The interrupt used is the DW-GDMA channel's, not the panel's: the panel
 * handle keeps one slot per callback and the LVGL adapter claims that slot
 * (with on_frame_buf_complete = NULL) right after the panel is created, which
 * is why the first version of this witness went silent after four frames.  The
 * DMA channel is registered exactly once, by IDF itself, with nobody else in
 * that path, so this hooks into that call instead (linker --wrap, see
 * main/CMakeLists.txt) and chains IDF's callback after the measurement. */
#define DSIW_GAP_MIN_US     18000u /* just over one frame period (16.3 ms at
                                    * 52 MHz): anything longer means the re-arm
                                    * was late by >= ~1.7 ms, i.e. the panel was
                                    * showing filler for that long            */
#define DSIW_GAP_RECENT_MS  5u     /* flash op that ended this close counts   */
#define DSIW_GAP_RING       8u     /* gaps queued for the poll task to log    */

typedef struct {
    uint32_t gap_us;
    uint32_t frames; /* frames since the previous gap (0 = the first one)      */
    uint32_t op_ms;
    uint8_t flash;
    uint8_t kind;
} dsiw_gap_evt_t;

static dsiw_gap_evt_t s_gap_ring[DSIW_GAP_RING];
static volatile uint32_t s_gap_head;
static volatile uint32_t s_gap_tail;
static volatile uint32_t s_gap_prev_us;
static volatile uint32_t s_gap_frames;
static volatile uint32_t s_gap_frames_since;
static volatile uint32_t s_gap_count;
static volatile uint32_t s_gap_max_us;
static volatile uint32_t s_gap_imax_us; /* worst interval of *all* frames  */
static volatile uint32_t s_gap_last_us;
static volatile uint32_t s_gap_with_flash;
static volatile uint32_t s_gap_lost;
static volatile bool s_gap_armed;
static volatile bool s_gap_have_prev;

static void gap_ring_push(const dsiw_gap_evt_t *evt)
{
    const uint32_t head = s_gap_head;
    const uint32_t next = (head + 1u) % DSIW_GAP_RING;
    if (next == s_gap_tail) {
        s_gap_lost++;
        return;
    }
    s_gap_ring[head] = *evt;
    __atomic_store_n(&s_gap_head, next, __ATOMIC_RELEASE);
}

static bool gap_ring_pop(dsiw_gap_evt_t *out)
{
    const uint32_t tail = s_gap_tail;
    if (tail == __atomic_load_n(&s_gap_head, __ATOMIC_ACQUIRE)) {
        return false;
    }
    *out = s_gap_ring[tail];
    s_gap_tail = (tail + 1u) % DSIW_GAP_RING;
    return true;
}

/* IDF exports this one (declared and defaulted to NULL nowhere); the panel
 * driver hands it to dw_gdma_channel_register_event_callbacks as
 * on_full_trans_done.  It is the function that restarts the scan-out, so the
 * witness runs in front of it and chains to it. */
extern bool mipi_dsi_dma_trans_done_cb(dw_gdma_channel_handle_t chan,
                                       const dw_gdma_trans_done_event_data_t *event_data, void *user_data);
extern esp_err_t __real_dw_gdma_channel_register_event_callbacks(dw_gdma_channel_handle_t chan,
                                                                 dw_gdma_event_callbacks_t *cbs, void *user_data);

/* Runs in the DW-GDMA interrupt once per frame, so it must stay in IRAM and
 * must not block.  The helper it calls is ordinary application code - this
 * build executes from PSRAM, and reaching it is safe by construction: a flash
 * operation masks interrupts, so one can never be in progress while this runs. */
static bool IRAM_ATTR dsiw_frame_tick(dw_gdma_channel_handle_t chan,
                                      const dw_gdma_trans_done_event_data_t *event_data, void *user_data)
{
    const uint32_t now_us = (uint32_t)esp_timer_get_time();
    const uint32_t delta = now_us - s_gap_prev_us; /* 0 until the first tick */

    s_gap_frames++;
    s_gap_frames_since++;
    s_gap_prev_us = now_us;

    if (s_gap_have_prev) {
        /* Sub-frame stalls are invisible to the gap counter (they are shorter
         * than one frame period) but they are still filler on the glass, so the
         * worst interval of every single frame is kept unconditionally: on a
         * healthy panel it stays a few hundred microseconds above the frame
         * period, and anything larger is time the glass spent on filler. */
        if (delta > s_gap_imax_us) {
            s_gap_imax_us = delta;
        }
        if (delta >= DSIW_GAP_MIN_US) {
            /* The scan-out was re-armed late: the panel was showing the bridge
             * filler for `delta` minus one frame period.  Ask the flash meter who
             * was to blame - it is the only consumer that can park the cores. */
            uint32_t kind = FLASH_OP_KIND_COUNT;
            uint32_t op_ms = 0;
            const bool flash = storage_guard_flash_op_probe(now_us / 1000u, DSIW_GAP_RECENT_MS, &kind, &op_ms);

            dsiw_gap_evt_t evt = {
                .gap_us = delta,
                .frames = s_gap_frames_since,
                .op_ms = op_ms,
                .flash = flash ? 1u : 0u,
                .kind = (uint8_t)kind,
            };
            gap_ring_push(&evt);

            s_gap_frames_since = 0u;
            s_gap_count++;
            s_gap_last_us = delta;
            if (delta > s_gap_max_us) {
                s_gap_max_us = delta;
            }
            if (flash) {
                s_gap_with_flash++;
            }
        }
    } else {
        s_gap_have_prev = true; /* the first tick is never a gap */
    }

    return mipi_dsi_dma_trans_done_cb(chan, event_data, user_data);
}

/* dsiw_frame_tick is in front of the driver's callback and runs on the DMA
 * interrupt path, which IDF only ever populates from here. */
esp_err_t __wrap_dw_gdma_channel_register_event_callbacks(dw_gdma_channel_handle_t chan,
                                                          dw_gdma_event_callbacks_t *cbs, void *user_data)
{
    if (cbs != NULL && cbs->on_full_trans_done == mipi_dsi_dma_trans_done_cb) {
        cbs->on_full_trans_done = dsiw_frame_tick;
        if (!s_gap_armed) {
            s_gap_armed = true;
            system_log_event("dsiwatch", "frame-gap witness armed on the scan-out DMA");
            ESP_LOGI(TAG, "frame-gap witness armed on the scan-out DMA");
        }
    }
    return __real_dw_gdma_channel_register_event_callbacks(chan, cbs, user_data);
}

void dsi_frame_gap_watch_stats(dsi_frame_gap_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    out->armed = s_gap_armed;
    out->frames = s_gap_frames;
    out->gaps = s_gap_count;
    out->max_interval_us = s_gap_imax_us;
    out->max_ms = s_gap_max_us / 1000u;
    out->last_ms = s_gap_last_us / 1000u;
    out->with_flash = s_gap_with_flash;
    out->lost = s_gap_lost;
}

static void poll_task(void *arg)
{
    (void)arg;
    uint32_t episode_start = 0;
    uint32_t episode_last = 0;
    uint32_t episode_events = 0;
    uint32_t last_log = 0;
    uint32_t last_phy_log = 0;
    uint32_t last_clobber_log = 0;
    uint32_t wd_last = 0;
    uint32_t phy_state_last = 0;
    bool phy_state_valid = false;
    bool phy_lock_prev = true;
    uint32_t fifo_zero_episode_last = 0;
    bool host_pld_empty_prev = false;
    uint32_t host_pld_run_start = 0;
    uint32_t st0_prev = 0;
    uint32_t st1_prev = 0;
    /* Every lane-state value already written to the log.  The DSI clock lane
     * toggles between LP and HS all the time, so the sampled mask alternates
     * (0x2f <-> 0x0a here) on nearly every 1 ms pass.  Logging each flip filled
     * the ~950 line log ring with identical lines twice a second and pushed
     * every real event out of it; a value is news the first time it is seen
     * (that is how a stuck or unknown lane state shows up), the counters below
     * keep the full rate in the diagnostics JSON. */
    uint64_t phy_lane_seen = 0;

    for (;;) {
        /* Frame gaps come first: they are the only artifact witness that still
         * works when the chip parked both cores inside the flash driver (the
         * interrupt that reports one has just run, so the ring is fresh). */
        dsiw_gap_evt_t gap_evt;
        while (gap_ring_pop(&gap_evt)) {
            system_log_event("screen", "SCREEN GAP %ums after %u frames, flash=%s op=%ums lost=%u",
                             (unsigned)(gap_evt.gap_us / 1000u), (unsigned)gap_evt.frames,
                             gap_evt.flash ? storage_guard_flash_op_kind_name(gap_evt.kind) : "none",
                             (unsigned)gap_evt.op_ms, (unsigned)s_gap_lost);
            ESP_LOGW(TAG, "SCREEN GAP %ums after %u frames, flash=%s op=%ums",
                     (unsigned)(gap_evt.gap_us / 1000u), (unsigned)gap_evt.frames,
                     gap_evt.flash ? storage_guard_flash_op_kind_name(gap_evt.kind) : "none",
                     (unsigned)gap_evt.op_ms);
        }

        const uint32_t raw = s_brg->int_raw.val;
        const uint32_t fifo = s_brg->fifo_flow_status.raw_buf_depth;
        const uint32_t now = now_ms();

        s_fifo_last = fifo;
        if (fifo < s_fifo_min) {
            s_fifo_min = fifo;
        }
        if (fifo < DSIW_FIFO_LOW) {
            s_fifo_low++;
        }

        /* Fine-grained FIFO sampling: the register is read back-to-back for
         * DSIW_FIFO_BURST_US instead of a fixed 32 reads, which turns a
         * sub-microsecond window into roughly a fifth of every millisecond - so
         * a starvation dip that neither the interrupt latch nor the 1 ms sample
         * above would catch still lands in f0/fe/fbmin.  A depth of exactly 0
         * means the bridge had nothing left to send: the DMA is asked to refill
         * at 768 of 1024 words, so that cannot happen on a fed link. */
        const uint32_t burst_deadline = esp_cpu_get_cycle_count() +
                                        (uint32_t)(DSIW_FIFO_BURST_US * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
        uint32_t burst_min = DSIW_FIFO_UNKNOWN;
        uint32_t burst_reads = 0;
        bool saw_zero = false;
        for (;;) {
            const uint32_t depth = s_brg->fifo_flow_status.raw_buf_depth;
            burst_reads++;
            if (depth < burst_min) {
                burst_min = depth;
            }
            if (depth == 0u) {
                s_fifo_zero++;
                saw_zero = true;
            }
            if (depth <= DSIW_FIFO_EDGE) {
                s_fifo_edge++;
            }
            if (burst_reads >= DSIW_FIFO_BURST_MAX ||
                (int32_t)(esp_cpu_get_cycle_count() - burst_deadline) >= 0) {
                break;
            }
        }
        if (burst_min < s_fifo_burst_min) {
            s_fifo_burst_min = burst_min;
        }
        if (saw_zero && (fifo_zero_episode_last == 0u ||
                         now - fifo_zero_episode_last > DSIW_EPISODE_GAP_MS)) {
            s_fifo_zero_runs++;
            char busy[48];
            busy_list(busy, sizeof(busy));
            system_log_event("dsiwatch", "bridge fifo dry #%u min=%u reads=%u busy=%s page=%s",
                             (unsigned)s_fifo_zero_runs, (unsigned)burst_min, (unsigned)burst_reads,
                             busy, ui_pages_current_id());
            ESP_LOGW(TAG, "bridge fifo dry #%u min=%u reads=%u busy=%s", (unsigned)s_fifo_zero_runs,
                     (unsigned)burst_min, (unsigned)burst_reads, busy);
            display_flash_watch_note("dsi:fifo0");
        }
        if (saw_zero) {
            fifo_zero_episode_last = now;
        }

        /* Host/PHY side: a lost link leaves the glass undriven, which shows up
         * as a light screen that no framebuffer and no backlight reading can
         * explain.  Read-only - never write a host register from here. */
        const uint32_t phy = s_host->phy_status.val;
        const uint32_t lane_state = (phy >> 2) & 0x3Fu; /* stop-state + ULPS bits 2..7 */
        s_phy_state_or |= lane_state;
        if (phy_state_valid && lane_state != phy_state_last) {
            s_phy_lane_chg++;
        }
        /* Only a lane state the log has never shown is worth a line: the LP/HS
         * oscillation of the clock lane is normal and was flooding the ring. */
        if (phy_state_valid && lane_state != phy_state_last && lane_state < 64u &&
            (phy_lane_seen & (1ull << lane_state)) == 0ull) {
            phy_lane_seen |= (1ull << lane_state);
            if (now - last_phy_log >= DSIW_PHY_LOG_MS) {
                last_phy_log = now;
                system_log_event("dsiwatch", "dsi phy lane state 0x%02x -> 0x%02x phy=0x%08x t=%ums",
                                 (unsigned)phy_state_last, (unsigned)lane_state, (unsigned)phy,
                                 (unsigned)now);
                ESP_LOGW(TAG, "dsi phy lane state 0x%02x -> 0x%02x phy=0x%08x", (unsigned)phy_state_last,
                         (unsigned)lane_state, (unsigned)phy);
                display_flash_watch_note("dsi:phylane");
            }
        }
        phy_state_last = lane_state;
        phy_state_valid = true;

        if ((phy & 1u) == 0u) {
            s_phy_unlocked++;
            if (phy_lock_prev) { /* 1 -> 0: the D-PHY just dropped lock */
                s_phy_unlocks++;
                s_phy_unlock_ms = now;
                if (now - last_phy_log >= DSIW_PHY_LOG_MS) {
                    last_phy_log = now;
                    system_log_event("dsiwatch", "dsi phy lock lost #%u phy=0x%08x unlocked=%u t=%ums",
                                     (unsigned)s_phy_unlocks, (unsigned)phy, (unsigned)s_phy_unlocked,
                                     (unsigned)now);
                    ESP_LOGW(TAG, "dsi phy lock lost #%u phy=0x%08x unlocked=%u", (unsigned)s_phy_unlocks,
                             (unsigned)phy, (unsigned)s_phy_unlocked);
                    display_flash_watch_note("dsi:phylock");
                }
            }
        }
        phy_lock_prev = (phy & 1u) != 0u;

        /* Second, independent starvation witness (read-only): dpi_buff_pld_empty
         * is the DWC host's pixel payload FIFO.  When it is dry the host has no
         * pixel to send for that line and pads with null packets, which the
         * panel cannot show - a light screen with a perfectly fed bridge.  It
         * also sets int_st1.dpi_buff_pld_under, which IDF's mask decides about;
         * the live bit below does not depend on that mask. */
        const bool host_pld_empty = (s_host->vid_pkt_status.val & DSIW_HOST_PLD_EMPTY_BIT) != 0u;
        if (host_pld_empty) {
            s_host_pld_empty++;
            s_host_pld_empty_ms = now;
            if (!host_pld_empty_prev) {
                s_host_pld_empty_runs++;
                host_pld_run_start = now;
            }
        } else if (host_pld_empty_prev) {
            /* End of a dry streak: how long was the host really without a pixel
             * to send?  The usual answer is "less than this 1 ms pass", which is
             * the gap between two lines, and only a long one means the line was
             * padded with null packets - a light screen with a fed bridge. */
            const uint32_t run = now - host_pld_run_start;
            if (run > s_host_pld_max_ms) {
                s_host_pld_max_ms = run;
            }
            /* Measured verdict (2026-09-18, running panel): this counter is NOT a
             * starvation witness and must not be logged as one.  The flag toggles
             * inside one line time - the host drains the payload FIFO in ~12 us of a
             * 16.7 us line - so a ~1 ms sampler reads it as a beat: ~213 runs/s, mean
             * 1.1 passes, some beat-locked streaks up to 57 ms.  A real 57 ms with no
             * pixel would push the DMA frame interval to ~57 ms, while the wrapped
             * scan-out witness measured imax=16622 us in the very same window, so the
             * streaks cannot mean "no pixel to send".  Keep the counters for trend. */
            if (run >= DSIW_HOST_PLD_LONG_MS) {
                s_host_pld_long_runs++;
            }
        }
        host_pld_empty_prev = host_pld_empty;

        const uint32_t st0 = s_host->int_st0.val;
        const uint32_t st1 = s_host->int_st1.val;
        /* Sticky status bits (int_st0 bit 20 never cleared) would hide a second,
         * identical event, so count the 0 -> 1 edges instead of the level. */
        const uint32_t edge0 = st0 & ~st0_prev;
        const uint32_t edge1 = st1 & ~st1_prev;
        s_host_st0_or |= st0;
        s_host_st1_or |= st1;
        if (edge0 || edge1) {
            s_host_st_edges += (uint32_t)(__builtin_popcount(edge0) + __builtin_popcount(edge1));
            host_status_note(st0, st1, edge0, edge1, phy, now);
        }
        st0_prev = st0;
        st1_prev = st1;

        /* Nothing in IDF re-inits the panel at runtime, but if something did,
         * it would silently undo the artifact mitigation and the cyan flash
         * would come back - so read the two registers back and re-assert them. */
        if (now - wd_last >= DSIW_WD_PERIOD_MS) {
            wd_last = now;
            const uint32_t rsv = s_brg->dpi_rsv_dpi_data.dpi_rsv_data;
            const uint32_t disc = s_brg->dpi_misc_config.fifo_underrun_discard_vcnt;
            const uint32_t ena = s_brg->int_ena.val;
            if (rsv != DSIW_RSV_PROBE || disc != DSIW_DISCARD_LINES || (ena & DSIW_UNDERRUN_BIT)) {
                s_clobbers++;
                if (now - last_clobber_log >= DSIW_LOG_INTERVAL_MS) {
                    last_clobber_log = now;
                    system_log_event("dsiwatch", "mitigation clobbered #%u rsv=%u disc=%u int_ena=0x%08x",
                                     (unsigned)s_clobbers, (unsigned)rsv, (unsigned)disc, (unsigned)ena);
                    ESP_LOGW(TAG, "mitigation clobbered #%u (rsv=%u disc=%u int_ena=0x%08x), re-applying",
                             (unsigned)s_clobbers, (unsigned)rsv, (unsigned)disc, (unsigned)ena);
                }
                underrun_artifact_mitigation();
                s_brg->int_ena.val &= ~DSIW_UNDERRUN_BIT;
            }
        }

        if (raw & DSIW_UNDERRUN_BIT) {
            /* Clear first: a new event can latch immediately after, and that is
             * exactly what the next 1 ms pass will pick up. */
            s_brg->int_clr.val = DSIW_UNDERRUN_BIT;

            s_underruns++;
            s_last_ms = now;
            for (int i = 0; i < DSI_BUS_SRC_COUNT; i++) {
                if (__atomic_load_n(&s_bus_depth[i], __ATOMIC_SEQ_CST) != 0u) {
                    s_corr[i]++;
                }
            }

            const bool new_episode = (episode_events == 0u) || (now - episode_last > DSIW_EPISODE_GAP_MS);
            if (new_episode) {
                if (episode_events > 1u) {
                    char busy[48];
                    busy_list(busy, sizeof(busy));
                    system_log_event("dsiwatch", "underrun burst: %u events in %ums busy=%s",
                                     (unsigned)episode_events, (unsigned)(episode_last - episode_start), busy);
                    ESP_LOGW(TAG, "underrun burst: %u events in %ums", (unsigned)episode_events,
                             (unsigned)(episode_last - episode_start));
                }
                s_bursts++;
                episode_start = now;
                episode_events = 0;
            }

            episode_last = now;
            episode_events++;
            if (episode_events > s_worst_burst) {
                s_worst_burst = episode_events;
            }

            if (new_episode && (now - last_log >= DSIW_LOG_INTERVAL_MS)) {
                char busy[48];
                busy_list(busy, sizeof(busy));
                last_log = now;
                /* The panel itself has already shown this as a light flash by
                 * now - this is the line that finally makes it visible. */
                system_log_event("dsiwatch", "dsi underrun #%u fifo=%u busy=%s page=%s",
                                 (unsigned)s_underruns, (unsigned)fifo, busy, ui_pages_current_id());
                ESP_LOGW(TAG, "dsi underrun #%u fifo=%u busy=%s page=%s", (unsigned)s_underruns,
                         (unsigned)fifo, busy, ui_pages_current_id());
                display_flash_watch_note("dsi:underrun");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DSIW_POLL_MS));
    }
}

/* See the DSIW_RSV_PROBE block above: the underrun itself is a DMA-bandwidth
 * problem, but *what it looks like* is decided by these two registers, and IDF
 * leaves them at their worst setting.  Must run after esp_lcd_new_panel_dpi()
 * (which writes fifo_underrun_discard_vcnt = h_size), i.e. after display_init(). */
static void underrun_artifact_mitigation(void)
{
    const uint32_t rsv_was = s_brg->dpi_rsv_dpi_data.dpi_rsv_data;
    const uint32_t disc_was = s_brg->dpi_misc_config.fifo_underrun_discard_vcnt;

    s_brg->dpi_rsv_dpi_data.dpi_rsv_data = DSIW_RSV_PROBE;
    s_brg->dpi_misc_config.fifo_underrun_discard_vcnt = DSIW_DISCARD_LINES;
    mipi_dsi_brg_ll_update_dpi_config(s_brg);

    system_log_event("dsiwatch", "underrun artifact: rsv_data %u -> %u, discard_vcnt %u -> %u lines",
                     (unsigned)rsv_was, (unsigned)DSIW_RSV_PROBE, (unsigned)disc_was,
                     (unsigned)DSIW_DISCARD_LINES);
    ESP_LOGI(TAG, "underrun artifact: rsv_data %u -> %u, discard_vcnt %u -> %u lines", (unsigned)rsv_was,
             (unsigned)DSIW_RSV_PROBE, (unsigned)disc_was, (unsigned)DSIW_DISCARD_LINES);
}

esp_err_t dsi_underrun_watch_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* If the DSI peripheral is not clocked (wrong panel variant, no DSI panel)
     * the register reads back as all ones.  Never arm against that. */
    const uint32_t ena = s_brg->int_ena.val;
    if (ena == 0xFFFFFFFFu) {
        ESP_LOGW(TAG, "DSI bridge not present, underrun watch disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Make the flash itself small and dark before counting it. */
    underrun_artifact_mitigation();

    s_brg->int_clr.val = DSIW_UNDERRUN_BIT;
    /* Take over the reporting from the IDF ISR.  Its only action for an
     * underrun is an ESP_DRAM_LOGE straight to the ROM console (invisible to
     * this firmware's log), and the bridge's VSYNC event is not implemented on
     * the P4 (MIPI_DSI_BRG_LL_EVENT_VSYNC == 0), while the "fake vsync" for the
     * LVGL port comes from the DW-GDMA done callback, not from here.  With the
     * enable bit cleared the raw status still latches, so nothing is lost. */
    s_brg->int_ena.val &= ~DSIW_UNDERRUN_BIT;
    s_brg->int_clr.val = DSIW_UNDERRUN_BIT;

    s_last_ms = 0;
    s_fifo_min = DSIW_FIFO_UNKNOWN;
    s_started = true;

    const BaseType_t created = xTaskCreate(poll_task, "dsiwatch", DSIW_TASK_STACK, NULL, DSIW_TASK_PRIO, NULL);
    if (created != pdPASS) {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    system_log_event("dsiwatch", "armed: int_ena=0x%08x fifo=%u poll=%dms disc=%u rsv=%u",
                     (unsigned)s_brg->int_ena.val, (unsigned)s_brg->fifo_flow_status.raw_buf_depth,
                     DSIW_POLL_MS, (unsigned)s_brg->dpi_misc_config.fifo_underrun_discard_vcnt,
                     (unsigned)s_brg->dpi_rsv_dpi_data.dpi_rsv_data);
    ESP_LOGI(TAG, "DSI underrun watch armed (fifo=%u, poll=%dms, discard=%u rsv=%u)",
             (unsigned)s_brg->fifo_flow_status.raw_buf_depth, DSIW_POLL_MS,
             (unsigned)s_brg->dpi_misc_config.fifo_underrun_discard_vcnt,
             (unsigned)s_brg->dpi_rsv_dpi_data.dpi_rsv_data);
    return ESP_OK;
}

int dsi_underrun_watch_summary(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    int n;
    if (!s_started) {
        n = snprintf(buf, len, "dsi_underruns=off");
        return n < 0 ? 0 : n;
    }
    if (s_underruns == 0u) {
        n = snprintf(buf, len, "dsi_underruns=0 fifo_min=%u fifo=%u", (unsigned)s_fifo_min,
                     (unsigned)s_fifo_last);
    } else {
        n = snprintf(buf, len, "dsi_underruns=%u bursts=%u worst=%u last=%us fifo_min=%u fifo=%u",
                     (unsigned)s_underruns, (unsigned)s_bursts, (unsigned)s_worst_burst,
                     (unsigned)((now_ms() - s_last_ms) / 1000u), (unsigned)s_fifo_min,
                     (unsigned)s_fifo_last);
    }
    if (n < 0 || (size_t)n >= len) {
        return n < 0 ? 0 : n;
    }
    int m = snprintf(buf + n, len - (size_t)n, " corr[cam=%u sd=%u shot=%u jpeg=%u wp=%u rnd=%u]",
                     (unsigned)s_corr[DSI_BUS_CAMERA], (unsigned)s_corr[DSI_BUS_SD],
                     (unsigned)s_corr[DSI_BUS_SCREENSHOT], (unsigned)s_corr[DSI_BUS_JPEG],
                     (unsigned)s_corr[DSI_BUS_WALLPAPER], (unsigned)s_corr[DSI_BUS_RENDER]);
    if (m > 0) {
        n += m;
    }
    if (n < 0 || (size_t)n >= len) {
        return n < 0 ? 0 : n;
    }
    m = snprintf(buf + n, len - (size_t)n, " busy[cam=%u sd=%u shot=%u jpeg=%u wp=%u rnd=%u]ms",
                 (unsigned)s_busy_ms[DSI_BUS_CAMERA], (unsigned)s_busy_ms[DSI_BUS_SD],
                 (unsigned)s_busy_ms[DSI_BUS_SCREENSHOT], (unsigned)s_busy_ms[DSI_BUS_JPEG],
                 (unsigned)s_busy_ms[DSI_BUS_WALLPAPER], (unsigned)s_busy_ms[DSI_BUS_RENDER]);
    if (m > 0) {
        n += m;
    }
    if (n < 0 || (size_t)n >= len) {
        return n < 0 ? 0 : n;
    }
    /* Read back from the hardware, not from a cache: `rsv` is the colour the
     * bridge paints on a starvation and `disc` the underrun mask, so this pair
     * is the proof that the probe below is really armed. */
    m = snprintf(buf + n, len - (size_t)n, " disc=%u rsv=%u fifo_low=%u",
                 (unsigned)s_brg->dpi_misc_config.fifo_underrun_discard_vcnt,
                 (unsigned)s_brg->dpi_rsv_dpi_data.dpi_rsv_data, (unsigned)s_fifo_low);
    if (m > 0) {
        n += m;
    }
    if (n < 0 || (size_t)n >= len) {
        return n < 0 ? 0 : n;
    }
    /* The content-free-flash witnesses.  unlk/unl = PHY lock lost / samples
     * spent unlocked, lane = ULPS or stop-state transitions, psx = the lane
     * states ever seen, st0/st1 = every host status bit ever latched, ste = how
     * often such a bit went 0 -> 1 (sticky bits would hide a repeat), f0/fe =
     * dense-window samples sitting at the very bottom of the bridge FIFO, fz =
     * how often that window caught a depth of exactly 0, fbmin = the lowest
     * depth a window ever caught, hpz/hpr = samples with the DWC host's own
     * pixel payload FIFO dry and how often it went dry, hpmax/hpl = the longest
     * continuous dry streak and how many streaks were long enough to pad real
     * lines with null packets (that is a light screen with a fed bridge),
     * clob = times the artifact mitigation had to be re-applied.  msk1 is read
     * back so it is visible whether the host's dpi_buff_pld_under status is even
     * unmasked. */
    m = snprintf(buf + n, len - (size_t)n,
                 " host[unlk=%u since=%us unl=%u lane=%u psx=0x%02x st0=0x%08x st1=0x%08x ste=%u]"
                 " f0=%u fe=%u fbmin=%u clob=%u fz=%u hpz=%u hpr=%u hpmax=%ums hpl=%u msk1=0x%08x",
                 (unsigned)s_phy_unlocks,
                 (unsigned)(s_phy_unlocks ? (now_ms() - s_phy_unlock_ms) / 1000u : 0u),
                 (unsigned)s_phy_unlocked, (unsigned)s_phy_lane_chg, (unsigned)s_phy_state_or,
                 (unsigned)s_host_st0_or, (unsigned)s_host_st1_or, (unsigned)s_host_st_edges,
                 (unsigned)s_fifo_zero, (unsigned)s_fifo_edge, (unsigned)s_fifo_burst_min,
                 (unsigned)s_clobbers, (unsigned)s_fifo_zero_runs, (unsigned)s_host_pld_empty,
                 (unsigned)s_host_pld_empty_runs, (unsigned)s_host_pld_max_ms,
                 (unsigned)s_host_pld_long_runs, (unsigned)s_host->int_msk1.val);
    if (m > 0) {
        n += m;
    }
    if (n < 0 || (size_t)n >= len) {
        return n < 0 ? 0 : n;
    }
    /* The artifact meter: `frames` must keep climbing (~60/s), every `gaps` is
     * one screen flash, `gmax`/`glast` how long the panel was left to the
     * filler, `imax` the worst interval of every frame (sub-frame filler shows
     * up here and nowhere else), `gflash` how many of them had a flash
     * operation in flight or just finished, `glost` gaps that happened while
     * the log was already busy. */
    m = snprintf(buf + n, len - (size_t)n,
                 " frames=%u gaps=%u gmax=%ums glast=%ums imax=%uus gflash=%u glost=%u",
                 (unsigned)s_gap_frames, (unsigned)s_gap_count, (unsigned)(s_gap_max_us / 1000u),
                 (unsigned)(s_gap_last_us / 1000u), (unsigned)s_gap_imax_us,
                 (unsigned)s_gap_with_flash, (unsigned)s_gap_lost);
    if (m > 0) {
        n += m;
    }
    return n < 0 ? 0 : n;
}

uint32_t dsi_underrun_watch_count(void)
{
    return s_started ? s_underruns : 0u;
}

uint32_t dsi_underrun_watch_bursts(void)
{
    return s_started ? s_bursts : 0u;
}

uint32_t dsi_underrun_watch_since_last_ms(void)
{
    return (s_started && s_underruns) ? (now_ms() - s_last_ms) : UINT32_MAX;
}

uint32_t dsi_underrun_watch_fifo_min(void)
{
    return s_fifo_min;
}

uint32_t dsi_underrun_watch_fifo_zero(void)
{
    return s_started ? s_fifo_zero : 0u;
}

uint32_t dsi_underrun_watch_phy_unlocks(void)
{
    return s_started ? s_phy_unlocks : 0u;
}

uint32_t dsi_underrun_watch_clobbers(void)
{
    return s_started ? s_clobbers : 0u;
}

uint32_t dsi_underrun_watch_fifo_zero_runs(void)
{
    return s_started ? s_fifo_zero_runs : 0u;
}

uint32_t dsi_underrun_watch_host_pld_empty(void)
{
    return s_started ? s_host_pld_empty_runs : 0u;
}

uint32_t dsi_underrun_watch_host_pld_max_ms(void)
{
    return s_started ? s_host_pld_max_ms : 0u;
}

uint32_t dsi_underrun_watch_host_pld_long_runs(void)
{
    return s_started ? s_host_pld_long_runs : 0u;
}

uint32_t dsi_underrun_watch_host_status_edges(void)
{
    return s_started ? s_host_st_edges : 0u;
}

#else /* !(7-inch panel variant on ESP32-P4) */

void dsi_frame_gap_watch_stats(dsi_frame_gap_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
}

esp_err_t dsi_underrun_watch_start(void)
{
    return ESP_OK;
}

void dsi_bus_activity_begin(dsi_bus_src_t src)
{
    (void)src;
}

void dsi_bus_activity_end(dsi_bus_src_t src)
{
    (void)src;
}

bool dsi_bus_activity_active(dsi_bus_src_t src)
{
    (void)src;
    return false;
}

int dsi_underrun_watch_summary(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return 0;
    }
    const int n = snprintf(buf, len, "dsi_underruns=off");
    return n < 0 ? 0 : n;
}

uint32_t dsi_underrun_watch_count(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_bursts(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_since_last_ms(void)
{
    return UINT32_MAX;
}

uint32_t dsi_underrun_watch_fifo_min(void)
{
    return DSIW_FIFO_UNKNOWN;
}

uint32_t dsi_underrun_watch_fifo_zero(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_phy_unlocks(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_clobbers(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_fifo_zero_runs(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_host_pld_empty(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_host_pld_max_ms(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_host_pld_long_runs(void)
{
    return 0;
}

uint32_t dsi_underrun_watch_host_status_edges(void)
{
    return 0;
}

#endif
