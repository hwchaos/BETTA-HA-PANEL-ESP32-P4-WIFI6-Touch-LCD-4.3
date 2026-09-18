/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Built-in OV5647 MIPI-CSI camera pipeline for the Waveshare
 * ESP32-P4-WIFI6-Touch-LCD-7B panel.
 *
 * Ported from the official Guition video_lcd_display demo (app_video.c) and
 * extended with a latest-frame cache + hardware JPEG snapshot + motion wake.
 *
 * The sensor sits on the shared BSP I2C bus (BSP_I2C_SCL / BSP_I2C_SDA) which
 * the touch controller already owns, so the SCCB handle is reused when the BSP
 * bus is up and the driver only falls back to creating its own bus otherwise.
 */
#include "camera/local_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#include "app_config.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "diag/dsi_underrun_watch.h"
#include "util/log_tags.h"

#define LOCAL_CAMERA_BUF_COUNT        2
#define LOCAL_CAMERA_STACK_SIZE       (5 * 1024)
#define LOCAL_CAMERA_TASK_PRIORITY    5
#define LOCAL_CAMERA_TASK_CORE        0

/* The capture loop fills the PSRAM frame cache (latest_frame) only while a
 * consumer asks for it.  Copying every 2nd frame around the clock consumed
 * ~60% of the PSRAM bandwidth the DSI scan-out needs (measured with the DSI
 * activity counters), and the motion detector samples the CSI buffers directly
 * so it never needed that copy.  A request arms the cache for
 * LOCAL_CAMERA_FRAME_HOLD_MS and waits up to LOCAL_CAMERA_FRAME_WAIT_MS for a
 * frame to actually land; LOCAL_CAMERA_FRAME_FRESH_MS is how stale a cached
 * frame may be before the caller waits for a newer one. */
#define LOCAL_CAMERA_FRAME_HOLD_MS   10000
#define LOCAL_CAMERA_FRAME_WAIT_MS   500
#define LOCAL_CAMERA_FRAME_FRESH_MS  500

/* Motion detector: sample a 32x32 luminance grid and compare against the
 * previous frame.  Fires the wake callback when the mean absolute difference
 * exceeds LOCAL_CAMERA_MOTION_THRESHOLD, throttled to once per second. */
#define LOCAL_CAMERA_MOTION_GRID       32
#define LOCAL_CAMERA_MOTION_THRESHOLD  8
#define LOCAL_CAMERA_MOTION_COOLDOWN_MS 1000
#define LOCAL_CAMERA_MOTION_START_DELAY_MS 2000
#define LOCAL_CAMERA_MOTION_CELLS \
    (LOCAL_CAMERA_MOTION_GRID * LOCAL_CAMERA_MOTION_GRID)

typedef struct {
    int fd;
    uint32_t width;    /* sensor/CSI capture resolution (native mode size) */
    uint32_t height;
    size_t buf_size;   /* full-resolution RGB565 frame size */

    bool downscale;    /* half-resolution output for JPEG cache/preview */
    uint32_t out_width;   /* effective output resolution */
    uint32_t out_height;
    size_t out_size;   /* effective output frame size */

    uint8_t *capture_bufs[LOCAL_CAMERA_BUF_COUNT];
    uint8_t *latest_frame;
    bool have_frame;
    volatile int64_t frame_demand_until_ms; /* capture loop fills the cache until this point */
    int64_t frame_copied_ms;                /* when latest_frame was last refreshed */
    SemaphoreHandle_t frame_mutex;

    jpeg_encoder_handle_t jpeg_engine;
    uint8_t *jpeg_out;
    size_t jpeg_out_size;

    TaskHandle_t task;
    TaskHandle_t watchdog_task;
    volatile bool stop_requested;

    bool motion_wake_enabled;
    uint8_t motion_threshold;
    uint8_t jpeg_quality;
    bool hflip;
    bool vflip;
    uint8_t *prev_luma;
    uint32_t frame_counter;
    int64_t last_motion_ms;
    local_camera_motion_cb_t motion_cb;
    void *motion_user;

    /* Motion detector tuning + live diagnostics (point 1). */
    uint8_t motion_min_area_pct;
    uint16_t motion_min_duration_ms;
    uint16_t motion_cooldown_ms;
    uint16_t motion_start_delay_ms;
    bool motion_ignore_lighting;
    uint8_t motion_zone_count;
    local_camera_motion_zone_t motion_zones[LOCAL_CAMERA_MOTION_MAX_ZONES];
    uint8_t motion_cell_zone[LOCAL_CAMERA_MOTION_CELLS];
    bool motion_zone_mask_valid;
    int64_t motion_started_at_ms;
    int64_t motion_active_since_ms;
    uint32_t motion_trigger_count;
    bool motion_last_ignored_lighting;
    uint8_t motion_last_level;
    uint8_t motion_last_changed_pct;
    uint8_t motion_zone_level[LOCAL_CAMERA_MOTION_MAX_ZONES];
    uint8_t motion_zone_changed_pct[LOCAL_CAMERA_MOTION_MAX_ZONES];
} local_camera_t;

static local_camera_t s_cam = {
    .fd = -1,
    .motion_threshold = LOCAL_CAMERA_MOTION_THRESHOLD,
    .motion_cooldown_ms = LOCAL_CAMERA_MOTION_COOLDOWN_MS,
    .motion_start_delay_ms = LOCAL_CAMERA_MOTION_START_DELAY_MS,
    .motion_ignore_lighting = true,
};

static int64_t now_ms(void)
{
    return (int64_t)esp_timer_get_time() / 1000;
}

static void log_camera_heap(const char *tag_msg)
{
    ESP_LOGW(TAG_CAMERA,
             "%s: total_free=%u largest=%u dma_free=%u dma_largest=%u internal_free=%u psram_free=%u",
             tag_msg,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void set_flip(bool hflip, bool vflip)
{
    if (s_cam.fd < 0) {
        return;
    }

    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[2];

    memset(&controls, 0, sizeof(controls));
    memset(control, 0, sizeof(control));

    /* Always send both controls so a flip can be disabled at runtime without
     * a full pipeline restart. */
    control[0].id = V4L2_CID_HFLIP;
    control[0].value = hflip ? 1 : 0;
    control[1].id = V4L2_CID_VFLIP;
    control[1].value = vflip ? 1 : 0;

    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 2;
    controls.controls = control;
    if (ioctl(s_cam.fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG_CAMERA, "Failed to apply camera flip (%d/%d)", hflip, vflip);
    }
}

static uint8_t rgb565_luma(uint16_t px)
{
    const uint32_t r = (px >> 11) & 0x1FU;
    const uint32_t g = (px >> 5) & 0x3FU;
    const uint32_t b = px & 0x1FU;
    /* 0..31 for r/b, 0..63 for g: scale to a rough 0..255 luma. */
    return (uint8_t)(((r * 30U) + (g * 30U) + (b * 11U)) >> 6);
}

/* Average four RGB565 pixels with rounding.  Produces noticeably cleaner
 * 2x downscales than nearest-neighbour sampling. */
static uint16_t rgb565_avg4(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    const uint32_t r = ((a >> 11) & 0x1FU) + ((b >> 11) & 0x1FU) +
                       ((c >> 11) & 0x1FU) + ((d >> 11) & 0x1FU);
    const uint32_t g = ((a >> 5) & 0x3FU) + ((b >> 5) & 0x3FU) +
                       ((c >> 5) & 0x3FU) + ((d >> 5) & 0x3FU);
    const uint32_t bl = (a & 0x1FU) + (b & 0x1FU) + (c & 0x1FU) + (d & 0x1FU);
    return (uint16_t)((((r + 2U) >> 2) << 11) |
                      (((g + 2U) >> 2) << 5) |
                      ((bl + 2U) >> 2));
}

/* 2x box-average downscale: src (w x h) RGB565 -> dst (w/2 x h/2) RGB565. */
static void downscale_rgb565_half(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    const uint32_t dw = w / 2;
    const uint32_t dh = h / 2;
    const size_t row_bytes = (size_t)w * 2;

    for (uint32_t y = 0; y < dh; y++) {
        const uint16_t *row0 = (const uint16_t *)(src + (size_t)(y * 2) * row_bytes);
        const uint16_t *row1 = (const uint16_t *)(src + (size_t)(y * 2 + 1) * row_bytes);
        uint16_t *drow = (uint16_t *)(dst + (size_t)y * dw * 2);
        for (uint32_t x = 0; x < dw; x++) {
            drow[x] = rgb565_avg4(row0[x * 2], row0[x * 2 + 1],
                                  row1[x * 2], row1[x * 2 + 1]);
        }
    }
}

/* Rebuild the cached grid-cell -> zone-index lookup (0..N-1, 0xFF = not in any
 * zone) from the percent-based zone rectangles.  Zone 0 always means the whole
 * frame when no zones are configured. */
static void motion_rebuild_zone_mask(void)
{
    const uint32_t g = LOCAL_CAMERA_MOTION_GRID;
    memset(s_cam.motion_cell_zone, 0xFF, sizeof(s_cam.motion_cell_zone));

    for (uint8_t z = 0; z < s_cam.motion_zone_count && z < LOCAL_CAMERA_MOTION_MAX_ZONES; z++) {
        const local_camera_motion_zone_t *zn = &s_cam.motion_zones[z];
        uint32_t x0 = ((uint32_t)zn->x * g) / 100;
        uint32_t y0 = ((uint32_t)zn->y * g) / 100;
        uint32_t x1 = ((uint32_t)(zn->x + zn->w) * g + 99) / 100;
        uint32_t y1 = ((uint32_t)(zn->y + zn->h) * g + 99) / 100;
        if (x1 > g) {
            x1 = g;
        }
        if (y1 > g) {
            y1 = g;
        }
        if (x0 >= x1) {
            x1 = x0 + 1;
        }
        if (y0 >= y1) {
            y1 = y0 + 1;
        }
        for (uint32_t cy = y0; cy < y1; cy++) {
            for (uint32_t cx = x0; cx < x1; cx++) {
                s_cam.motion_cell_zone[cy * g + cx] = z;
            }
        }
    }

    s_cam.motion_zone_mask_valid = (s_cam.motion_zone_count > 0);
}

static void motion_detect(const uint8_t *frame)
{
    if (s_cam.prev_luma == NULL || frame == NULL) {
        return;
    }

    const int64_t now = now_ms();
    /* Grace period after STREAMON so AE/AWB converge before detection. */
    if (now - s_cam.motion_started_at_ms < (int64_t)s_cam.motion_start_delay_ms) {
        return;
    }

    const uint32_t step_x = s_cam.width / LOCAL_CAMERA_MOTION_GRID;
    const uint32_t step_y = s_cam.height / LOCAL_CAMERA_MOTION_GRID;
    if (step_x == 0 || step_y == 0) {
        return;
    }

    const bool has_zones = s_cam.motion_zone_mask_valid;
    const uint8_t threshold = s_cam.motion_threshold;
    const uint8_t *mask = has_zones ? s_cam.motion_cell_zone : NULL;

    uint32_t diff_sum = 0;
    int32_t signed_sum = 0;
    uint32_t active_cells = 0;
    uint32_t changed_cells = 0;
    uint32_t zone_diff[LOCAL_CAMERA_MOTION_MAX_ZONES] = {0};
    uint32_t zone_cells[LOCAL_CAMERA_MOTION_MAX_ZONES] = {0};
    uint32_t zone_changed[LOCAL_CAMERA_MOTION_MAX_ZONES] = {0};

    uint8_t *prev = s_cam.prev_luma;
    for (uint32_t gy = 0; gy < LOCAL_CAMERA_MOTION_GRID; gy++) {
        const uint32_t y = (gy * step_y) + (step_y / 2);
        const uint8_t *row = frame + ((size_t)y * s_cam.width * 2);
        for (uint32_t gx = 0; gx < LOCAL_CAMERA_MOTION_GRID; gx++) {
            const uint8_t z = has_zones ? mask[gy * LOCAL_CAMERA_MOTION_GRID + gx] : 0;
            const uint32_t x = (gx * step_x) + (step_x / 2);
            const uint8_t luma = rgb565_luma((uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8)));
            const uint8_t old = *prev;
            *prev = luma;
            prev++;

            if (z == 0xFF) {
                continue;
            }

            const int16_t delta = (int16_t)luma - (int16_t)old;
            const uint8_t abs_delta = delta < 0 ? (uint8_t)(-delta) : (uint8_t)delta;
            diff_sum += abs_delta;
            signed_sum += delta;
            active_cells++;
            if (abs_delta >= threshold) {
                changed_cells++;
            }
            if (z < LOCAL_CAMERA_MOTION_MAX_ZONES) {
                zone_diff[z] += abs_delta;
                zone_cells[z]++;
                if (abs_delta >= threshold) {
                    zone_changed[z]++;
                }
            }
        }
    }

    if (active_cells == 0) {
        return;
    }

    const uint32_t mean_diff = diff_sum / active_cells;
    const uint32_t changed_pct = (changed_cells * 100) / active_cells;

    /* Publish live diagnostics for /api/camera/motion and the web editor. */
    s_cam.motion_last_level = (uint8_t)(mean_diff > 255 ? 255 : mean_diff);
    s_cam.motion_last_changed_pct = (uint8_t)(changed_pct > 100 ? 100 : changed_pct);
    for (uint8_t z = 0; z < LOCAL_CAMERA_MOTION_MAX_ZONES; z++) {
        s_cam.motion_zone_level[z] = zone_cells[z] ? (uint8_t)(zone_diff[z] / zone_cells[z]) : 0;
        s_cam.motion_zone_changed_pct[z] =
            zone_cells[z] ? (uint8_t)((zone_changed[z] * 100) / zone_cells[z]) : 0;
    }

    /* Global-brightness filter: when most cells move in the same direction by a
     * similar amount it is a lighting change (lights on/off, AE step), not a
     * person.  A moving subject produces mixed-direction deltas that cancel. */
    s_cam.motion_last_ignored_lighting = false;
    if (s_cam.motion_ignore_lighting && changed_pct >= 60) {
        const int32_t mean_signed = signed_sum / (int32_t)active_cells;
        const int32_t abs_signed = mean_signed < 0 ? -mean_signed : mean_signed;
        if ((int32_t)mean_diff > 0 && abs_signed * 10 >= (int32_t)mean_diff * 7) {
            s_cam.motion_last_ignored_lighting = true;
            s_cam.motion_active_since_ms = 0;
            return;
        }
    }

    bool above = (mean_diff >= threshold);
    if (s_cam.motion_min_area_pct > 0 && changed_pct < s_cam.motion_min_area_pct) {
        above = false;
    }

    if (!above) {
        s_cam.motion_active_since_ms = 0;
        return;
    }

    /* Debounce: the change has to persist for min_duration_ms before firing. */
    if (s_cam.motion_active_since_ms == 0) {
        s_cam.motion_active_since_ms = now;
    }
    if (now - s_cam.motion_active_since_ms < (int64_t)s_cam.motion_min_duration_ms) {
        return;
    }

    if (now - s_cam.last_motion_ms < (int64_t)s_cam.motion_cooldown_ms) {
        return;
    }
    s_cam.last_motion_ms = now;
    s_cam.motion_trigger_count++;

    if (s_cam.motion_cb != NULL) {
        s_cam.motion_cb(s_cam.motion_user);
    }
}

static void stream_task(void *arg)
{
    (void)arg;
    uint32_t dqbuf_failures = 0;

    while (!s_cam.stop_requested) {
        struct v4l2_buffer vb;
        memset(&vb, 0, sizeof(vb));
        vb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vb.memory = V4L2_MEMORY_USERPTR;

        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &vb) != 0) {
            /* VIDIOC_DQBUF normally blocks (never errors) when no frame has
             * arrived, so this error path is rare.  The no-frames condition
             * is reported by the frame-arrival watchdog task instead. */
            dqbuf_failures++;
            if (dqbuf_failures == 100) {
                ESP_LOGW(TAG_CAMERA, "DQBUF failing (errno=%d): sensor not streaming", errno);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        dqbuf_failures = 0;

        s_cam.frame_counter++;

        /* Copy every 2nd frame to the latest-frame cache so the JPEG snapshot
         * does not contend with the DMA buffers.  Only done while something
         * actually wants a frame (JPEG snapshot, MJPEG stream, on-panel
         * preview): the copy is the camera's heaviest PSRAM burst and the DSI
         * scan-out starves when it runs permanently in the background. */
        if ((s_cam.frame_counter & 1U) == 0U && now_ms() < s_cam.frame_demand_until_ms) {
            if (xSemaphoreTake(s_cam.frame_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                /* Copying a full frame out of the CSI buffers is the camera's
                 * biggest PSRAM burst; bracketed so an underrun that lands here
                 * is reported as "busy=cam". */
                dsi_bus_activity_begin(DSI_BUS_CAMERA);
                if (s_cam.downscale) {
                    downscale_rgb565_half(s_cam.capture_bufs[vb.index], s_cam.latest_frame,
                                          s_cam.width, s_cam.height);
                } else {
                    memcpy(s_cam.latest_frame, s_cam.capture_bufs[vb.index], s_cam.out_size);
                }
                dsi_bus_activity_end(DSI_BUS_CAMERA);
                s_cam.have_frame = true;
                s_cam.frame_copied_ms = now_ms();
                xSemaphoreGive(s_cam.frame_mutex);
            }
        }

        if (s_cam.motion_wake_enabled && (s_cam.frame_counter % 4U) == 0U) {
            motion_detect(s_cam.capture_bufs[vb.index]);
        }

        /* V4L2 USERPTR requeue: VIDIOC_DQBUF fills index/bytesused but leaves
         * m.userptr and length untouched, so restore them or QBUF rejects the
         * buffer (ESP_ERR_INVALID_ARG) and the stream stalls after 1 frame. */
        vb.m.userptr = (unsigned long)s_cam.capture_bufs[vb.index];
        vb.length = s_cam.buf_size;

        if (ioctl(s_cam.fd, VIDIOC_QBUF, &vb) != 0) {
            ESP_LOGW(TAG_CAMERA, "Failed to requeue camera buffer (errno=%d)", errno);
        }
    }

    vTaskDelete(NULL);
}

/* Frame-arrival watchdog: VIDIOC_DQBUF blocks forever when the CSI pipeline
 * delivers nothing, so it can never report the "no frames" condition.  This
 * task samples the frame counter and, if it has not advanced, logs the state
 * plus the internal-DMA heap (the resource ESP-Hosted WiFi is starving) so we
 * can see exactly what the pipeline is stuck on.  It self-terminates. */
static void frame_watchdog_task(void *arg)
{
    (void)arg;
    const uint32_t check_ms = 3000;
    uint32_t prev_counter = s_cam.frame_counter;

    for (int round = 0; round < 4 && !s_cam.stop_requested; round++) {
        vTaskDelay(pdMS_TO_TICKS(check_ms));
        if (s_cam.stop_requested) {
            break;
        }

        const uint32_t counter = s_cam.frame_counter;
        if (counter == 0) {
            ESP_LOGW(TAG_CAMERA, "Camera no frames after %" PRIu32 "s (streaming but DQBUF blocking)",
                     (uint32_t)((round + 1) * check_ms / 1000));
            log_camera_heap("no-frame heap");
        } else if (counter == prev_counter) {
            ESP_LOGW(TAG_CAMERA, "Camera stream stalled (frame_counter=%" PRIu32 ")", counter);
            log_camera_heap("stall heap");
        }
        prev_counter = counter;
    }

    s_cam.watchdog_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t open_device(void)
{
    const char *dev = ESP_VIDEO_MIPI_CSI_DEVICE_NAME;
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG_CAMERA, "Open %s failed", dev);
        return ESP_FAIL;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG_CAMERA, "VIDIOC_G_FMT failed");
        close(fd);
        return ESP_FAIL;
    }

    s_cam.width = fmt.fmt.pix.width;
    s_cam.height = fmt.fmt.pix.height;
    s_cam.out_width = s_cam.downscale ? (s_cam.width / 2) : s_cam.width;
    s_cam.out_height = s_cam.downscale ? (s_cam.height / 2) : s_cam.height;
    s_cam.out_size = (size_t)s_cam.out_width * s_cam.out_height * 2;

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format request = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .fmt.pix.width = fmt.fmt.pix.width,
            .fmt.pix.height = fmt.fmt.pix.height,
            .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
        };
        if (ioctl(fd, VIDIOC_S_FMT, &request) != 0) {
            ESP_LOGE(TAG_CAMERA, "VIDIOC_S_FMT RGB565 failed");
            close(fd);
            return ESP_FAIL;
        }
    }

    s_cam.buf_size = (size_t)s_cam.width * s_cam.height * 2;
    s_cam.fd = fd;

    ESP_LOGI(TAG_CAMERA, "Camera opened: %" PRIu32 "x%" PRIu32 " RGB565 (%u bytes/frame), output %" PRIu32 "x%" PRIu32,
             s_cam.width, s_cam.height, (unsigned)s_cam.buf_size,
             s_cam.out_width, s_cam.out_height);

    return ESP_OK;
}

static esp_err_t setup_buffers(void)
{
    /* ESP32-P4 data cache line size; USERPTR DMA buffers must be cache-line aligned. */
    const size_t cache_line = 64;

    for (int i = 0; i < LOCAL_CAMERA_BUF_COUNT; i++) {
        s_cam.capture_bufs[i] = heap_caps_aligned_calloc(cache_line, 1, s_cam.buf_size, MALLOC_CAP_SPIRAM);
        if (s_cam.capture_bufs[i] == NULL) {
            ESP_LOGE(TAG_CAMERA, "Failed to allocate capture buffer %d (%u bytes)", i, (unsigned)s_cam.buf_size);
            return ESP_ERR_NO_MEM;
        }
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = LOCAL_CAMERA_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG_CAMERA, "VIDIOC_REQBUFS failed");
        return ESP_FAIL;
    }

    for (int i = 0; i < LOCAL_CAMERA_BUF_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = i;

        if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG_CAMERA, "VIDIOC_QUERYBUF %d failed", i);
            return ESP_FAIL;
        }
        if (buf.length > s_cam.buf_size) {
            ESP_LOGE(TAG_CAMERA, "Driver wants %u bytes but only %u allocated", buf.length, (unsigned)s_cam.buf_size);
            return ESP_ERR_NO_MEM;
        }

        buf.m.userptr = (unsigned long)s_cam.capture_bufs[i];
        buf.length = s_cam.buf_size;
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG_CAMERA, "VIDIOC_QBUF %d failed", i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t setup_jpeg(void)
{
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    if (jpeg_new_encoder_engine(&eng_cfg, &s_cam.jpeg_engine) != ESP_OK) {
        ESP_LOGE(TAG_CAMERA, "jpeg_new_encoder_engine failed");
        return ESP_FAIL;
    }

    jpeg_encode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated = 0;
    /* RGB565 -> JPEG at quality <= 95 compresses well under 4:1. */
    const size_t want = (s_cam.out_size / 4) + 4096;
    s_cam.jpeg_out = jpeg_alloc_encoder_mem(want, &out_cfg, &allocated);
    if (s_cam.jpeg_out == NULL) {
        ESP_LOGE(TAG_CAMERA, "jpeg_alloc_encoder_mem failed");
        return ESP_ERR_NO_MEM;
    }
    s_cam.jpeg_out_size = allocated;
    return ESP_OK;
}

esp_err_t local_camera_start(void)
{
    if (s_cam.task != NULL) {
        return ESP_OK;
    }

    esp_video_init_csi_config_t csi_config[] = {
        {
            .sccb_config = {
                .init_sccb = true,
                .i2c_config = {
                    .port = BSP_I2C_NUM,
                    .scl_pin = BSP_I2C_SCL,
                    .sda_pin = BSP_I2C_SDA,
                },
                .freq = 100000,
            },
            .reset_pin = -1,
            .pwdn_pin = -1,
        },
    };

    /* The BSP already brings up the I2C bus for the touch controller; the
     * SCCB of the sensor hangs on the same pins, so share that bus. */
    (void)bsp_i2c_init();
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus != NULL) {
        csi_config[0].sccb_config.init_sccb = false;
        csi_config[0].sccb_config.i2c_handle = bus;
    }

    esp_video_init_config_t cam_config = {
        .csi = csi_config,
    };
    esp_err_t err = esp_video_init(&cam_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_CAMERA, "esp_video_init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (open_device() != ESP_OK) {
        goto fail;
    }

    s_cam.frame_mutex = xSemaphoreCreateMutex();
    if (s_cam.frame_mutex == NULL) {
        ESP_LOGE(TAG_CAMERA, "Failed to create frame mutex");
        goto fail;
    }

    const size_t cache_line = 64;
    s_cam.latest_frame = heap_caps_aligned_calloc(cache_line, 1, s_cam.out_size, MALLOC_CAP_SPIRAM);
    if (s_cam.latest_frame == NULL) {
        ESP_LOGE(TAG_CAMERA, "Failed to allocate latest-frame buffer");
        goto fail;
    }
    s_cam.prev_luma = heap_caps_calloc(LOCAL_CAMERA_MOTION_GRID * LOCAL_CAMERA_MOTION_GRID, 1, MALLOC_CAP_INTERNAL);
    if (s_cam.prev_luma == NULL) {
        ESP_LOGE(TAG_CAMERA, "Failed to allocate motion grid");
        goto fail;
    }

    if (setup_buffers() != ESP_OK) {
        goto fail;
    }
    if (setup_jpeg() != ESP_OK) {
        goto fail;
    }

    s_cam.motion_wake_enabled = CONFIG_APP_LOCAL_CAMERA_MOTION_WAKE;
    s_cam.motion_threshold = LOCAL_CAMERA_MOTION_THRESHOLD;
    s_cam.jpeg_quality = CONFIG_APP_LOCAL_CAMERA_JPEG_QUALITY;
#ifdef CONFIG_APP_LOCAL_CAMERA_HFLIP
    s_cam.hflip = CONFIG_APP_LOCAL_CAMERA_HFLIP;
#else
    s_cam.hflip = false;
#endif
#ifdef CONFIG_APP_LOCAL_CAMERA_VFLIP
    s_cam.vflip = CONFIG_APP_LOCAL_CAMERA_VFLIP;
#else
    s_cam.vflip = false;
#endif
    set_flip(s_cam.hflip, s_cam.vflip);

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG_CAMERA, "VIDIOC_STREAMON failed");
        goto fail;
    }

    /* Reset the motion detector's per-session state; the tuning itself
     * (threshold/cooldown/zones/…) persists in s_cam across restarts. */
    s_cam.motion_started_at_ms = now_ms();
    s_cam.motion_active_since_ms = 0;

    s_cam.stop_requested = false;
    if (xTaskCreatePinnedToCore(stream_task, "local_camera", LOCAL_CAMERA_STACK_SIZE, NULL,
                                LOCAL_CAMERA_TASK_PRIORITY, &s_cam.task, LOCAL_CAMERA_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG_CAMERA, "Failed to create stream task");
        s_cam.task = NULL;
        goto fail;
    }

    if (xTaskCreatePinnedToCore(frame_watchdog_task, "cam_watchdog", 2048, NULL,
                                LOCAL_CAMERA_TASK_PRIORITY - 1, &s_cam.watchdog_task,
                                LOCAL_CAMERA_TASK_CORE) != pdPASS) {
        /* Non-fatal: the diagnostic watchdog is optional. */
        s_cam.watchdog_task = NULL;
    }

    ESP_LOGI(TAG_CAMERA, "Local camera started (%" PRIu32 "x%" PRIu32 ")", s_cam.width, s_cam.height);
    return ESP_OK;

fail:
    local_camera_deinit();
    return ESP_FAIL;
}

esp_err_t local_camera_stop(void)
{
    if (s_cam.task == NULL) {
        return ESP_OK;
    }

    s_cam.stop_requested = true;
    /* Give the stream task a moment to observe the flag and STREAMOFF. */
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_cam.fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
    }

    /* The stream task deletes itself; clear the handle. */
    s_cam.task = NULL;

    /* The watchdog self-terminates, but delete it eagerly so a fast restart
     * cannot overlap two watchdogs. */
    if (s_cam.watchdog_task != NULL) {
        vTaskDelete(s_cam.watchdog_task);
        s_cam.watchdog_task = NULL;
    }
    return ESP_OK;
}

void local_camera_deinit(void)
{
    local_camera_stop();

    if (s_cam.jpeg_engine != NULL) {
        jpeg_del_encoder_engine(s_cam.jpeg_engine);
        s_cam.jpeg_engine = NULL;
    }
    if (s_cam.jpeg_out != NULL) {
        free(s_cam.jpeg_out);
        s_cam.jpeg_out = NULL;
        s_cam.jpeg_out_size = 0;
    }
    if (s_cam.latest_frame != NULL) {
        heap_caps_free(s_cam.latest_frame);
        s_cam.latest_frame = NULL;
    }
    if (s_cam.prev_luma != NULL) {
        heap_caps_free(s_cam.prev_luma);
        s_cam.prev_luma = NULL;
    }
    for (int i = 0; i < LOCAL_CAMERA_BUF_COUNT; i++) {
        if (s_cam.capture_bufs[i] != NULL) {
            heap_caps_free(s_cam.capture_bufs[i]);
            s_cam.capture_bufs[i] = NULL;
        }
    }
    if (s_cam.frame_mutex != NULL) {
        vSemaphoreDelete(s_cam.frame_mutex);
        s_cam.frame_mutex = NULL;
    }
    if (s_cam.fd >= 0) {
        close(s_cam.fd);
        s_cam.fd = -1;
    }
    s_cam.have_frame = false;
    s_cam.frame_demand_until_ms = 0;
    s_cam.frame_copied_ms = 0;

    /* Tear down the esp_video CSI/ISP device tree. Without this the ISP
     * device stays registered and a later local_camera_start() fails with
     * "video name=ISP id=20 has been registered", so the camera could only
     * ever be started once per boot (resolution change / enable-disable
     * cycles would never come back up). */
    esp_video_deinit();
}

bool local_camera_is_running(void)
{
    return s_cam.task != NULL;
}

void local_camera_demand_frames(uint32_t hold_ms)
{
    const int64_t until = now_ms() + (int64_t)hold_ms;
    if (until > s_cam.frame_demand_until_ms) {
        s_cam.frame_demand_until_ms = until;
    }
}

/* Arm the frame cache and, if the cached frame is stale (nothing has read the
 * camera recently, so the capture loop stopped copying), wait briefly for a
 * fresh one.  Returns true when a frame is available. */
static bool frame_ready(uint32_t timeout_ms)
{
    local_camera_demand_frames(LOCAL_CAMERA_FRAME_HOLD_MS);

    if (s_cam.latest_frame == NULL || s_cam.stop_requested) {
        return false;
    }

    const int64_t deadline = now_ms() + (int64_t)timeout_ms;
    for (;;) {
        const int64_t age = now_ms() - s_cam.frame_copied_ms;
        if (s_cam.have_frame && s_cam.frame_copied_ms != 0 &&
            age <= LOCAL_CAMERA_FRAME_FRESH_MS) {
            return true;
        }
        if (now_ms() >= deadline || s_cam.stop_requested || !local_camera_is_running()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return s_cam.have_frame;
}

void local_camera_set_motion_wake(bool enabled)
{
    s_cam.motion_wake_enabled = enabled;
}

void local_camera_set_motion_threshold(uint8_t threshold)
{
    if (threshold < 1) {
        threshold = 1;
    }
    if (threshold > 64) {
        threshold = 64;
    }
    s_cam.motion_threshold = threshold;
}

static uint8_t motion_clamp_zone_byte(uint8_t v, uint8_t max)
{
    return v > max ? max : v;
}

void local_camera_set_motion_config(const local_camera_motion_config_t *config)
{
    if (config == NULL) {
        return;
    }

    uint8_t threshold = config->threshold;
    if (threshold < 1) {
        threshold = 1;
    }
    if (threshold > 64) {
        threshold = 64;
    }
    s_cam.motion_threshold = threshold;

    s_cam.motion_min_area_pct = config->min_area_pct > 100 ? 100 : config->min_area_pct;
    s_cam.motion_min_duration_ms = config->min_duration_ms > 1000 ? 1000 : config->min_duration_ms;
    s_cam.motion_cooldown_ms = config->cooldown_ms > 30000 ? 30000 : config->cooldown_ms;
    s_cam.motion_start_delay_ms = config->start_delay_ms > 10000 ? 10000 : config->start_delay_ms;
    s_cam.motion_ignore_lighting = config->ignore_lighting;

    s_cam.motion_zone_count = config->zone_count > LOCAL_CAMERA_MOTION_MAX_ZONES
                                  ? LOCAL_CAMERA_MOTION_MAX_ZONES
                                  : config->zone_count;
    for (uint8_t z = 0; z < LOCAL_CAMERA_MOTION_MAX_ZONES; z++) {
        s_cam.motion_zones[z].x = motion_clamp_zone_byte(config->zones[z].x, 100);
        s_cam.motion_zones[z].y = motion_clamp_zone_byte(config->zones[z].y, 100);
        s_cam.motion_zones[z].w = motion_clamp_zone_byte(config->zones[z].w, 100);
        s_cam.motion_zones[z].h = motion_clamp_zone_byte(config->zones[z].h, 100);
    }

    motion_rebuild_zone_mask();
    /* A changed zone layout invalidates any in-progress debounce window. */
    s_cam.motion_active_since_ms = 0;
}

void local_camera_get_motion_config(local_camera_motion_config_t *config)
{
    if (config == NULL) {
        return;
    }

    config->threshold = s_cam.motion_threshold;
    config->min_area_pct = s_cam.motion_min_area_pct;
    config->min_duration_ms = s_cam.motion_min_duration_ms;
    config->cooldown_ms = s_cam.motion_cooldown_ms;
    config->start_delay_ms = s_cam.motion_start_delay_ms;
    config->ignore_lighting = s_cam.motion_ignore_lighting;
    config->zone_count = s_cam.motion_zone_count;
    for (uint8_t z = 0; z < LOCAL_CAMERA_MOTION_MAX_ZONES; z++) {
        config->zones[z] = s_cam.motion_zones[z];
    }
}

void local_camera_get_motion_status(local_camera_motion_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->threshold = s_cam.motion_threshold;
    status->last_level = s_cam.motion_last_level;
    status->last_changed_pct = s_cam.motion_last_changed_pct;
    status->last_ignored_lighting = s_cam.motion_last_ignored_lighting;
    status->active = (s_cam.motion_active_since_ms != 0);
    status->trigger_count = s_cam.motion_trigger_count;
    status->last_trigger_ms = s_cam.last_motion_ms;
    status->zone_count = s_cam.motion_zone_count;
    for (uint8_t z = 0; z < LOCAL_CAMERA_MOTION_MAX_ZONES; z++) {
        status->zone_level[z] = s_cam.motion_zone_level[z];
        status->zone_changed_pct[z] = s_cam.motion_zone_changed_pct[z];
    }
}

void local_camera_set_jpeg_quality(uint8_t quality)
{
    if (quality < 10) {
        quality = 10;
    }
    if (quality > 95) {
        quality = 95;
    }
    s_cam.jpeg_quality = quality;
}

void local_camera_set_flip(bool hflip, bool vflip)
{
    s_cam.hflip = hflip;
    s_cam.vflip = vflip;
    set_flip(hflip, vflip);
}

bool local_camera_get_motion_wake(void)
{
    return s_cam.motion_wake_enabled;
}

uint8_t local_camera_get_motion_threshold(void)
{
    return s_cam.motion_threshold;
}

uint8_t local_camera_get_jpeg_quality(void)
{
    return s_cam.jpeg_quality;
}

bool local_camera_get_hflip(void)
{
    return s_cam.hflip;
}

bool local_camera_get_vflip(void)
{
    return s_cam.vflip;
}

esp_err_t local_camera_snapshot_jpeg(uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    if (!local_camera_is_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!frame_ready(LOCAL_CAMERA_FRAME_WAIT_MS) || s_cam.latest_frame == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cam.frame_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    jpeg_encode_cfg_t cfg = {
        .height = s_cam.out_height,
        .width = s_cam.out_width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = s_cam.jpeg_quality,
        .pixel_reverse = false,
    };

    uint32_t out_size = 0;
    dsi_bus_activity_begin(DSI_BUS_JPEG);
    esp_err_t err = jpeg_encoder_process(s_cam.jpeg_engine, &cfg, s_cam.latest_frame, s_cam.out_size,
                                         s_cam.jpeg_out, s_cam.jpeg_out_size, &out_size);
    dsi_bus_activity_end(DSI_BUS_JPEG);
    xSemaphoreGive(s_cam.frame_mutex);

    if (err != ESP_OK || out_size == 0) {
        ESP_LOGE(TAG_CAMERA, "JPEG encode failed: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    uint8_t *copy = malloc(out_size);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_cam.jpeg_out, out_size);
    *out_buf = copy;
    *out_len = out_size;
    return ESP_OK;
}

esp_err_t local_camera_register_motion_cb(local_camera_motion_cb_t cb, void *user_data)
{
    s_cam.motion_cb = cb;
    s_cam.motion_user = user_data;
    return ESP_OK;
}

void local_camera_set_resolution(int resolution)
{
    s_cam.downscale = (resolution > 0);
}

int local_camera_get_resolution(void)
{
    return s_cam.downscale ? 1 : 0;
}

int local_camera_width(void)
{
    return (int)(s_cam.out_width > 0 ? s_cam.out_width : s_cam.width);
}

int local_camera_height(void)
{
    return (int)(s_cam.out_height > 0 ? s_cam.out_height : s_cam.height);
}

esp_err_t local_camera_apply_settings(bool enabled, bool motion_wake, uint8_t motion_threshold,
                                      uint8_t jpeg_quality, bool hflip, bool vflip,
                                      int resolution)
{
    const bool want_downscale = (resolution > 0);
    const bool running = local_camera_is_running();

    /* A resolution change requires a full teardown + restart so the frame
     * cache and JPEG buffers are reallocated for the new output size. */
    if (running && (want_downscale != s_cam.downscale)) {
        local_camera_deinit();
        s_cam.downscale = want_downscale;
        s_cam.out_width = 0;
        s_cam.out_height = 0;
        s_cam.out_size = 0;
        /* Fall through: a fresh start below re-creates the pipeline. */
    } else {
        s_cam.downscale = want_downscale;
    }

    esp_err_t err = ESP_OK;
    if (enabled) {
        err = local_camera_start();
        if (err == ESP_OK) {
            /* local_camera_start() resets parameters to Kconfig defaults, so
             * re-apply the persisted values on top of the fresh pipeline. */
            local_camera_register_motion_cb(s_cam.motion_cb, s_cam.motion_user);
            local_camera_set_motion_wake(motion_wake);
            local_camera_set_motion_threshold(motion_threshold);
            local_camera_set_jpeg_quality(jpeg_quality);
            local_camera_set_flip(hflip, vflip);
        }
    } else {
        /* Disabling must release the pipeline's PSRAM/DMA buffers too, not
         * just stop the stream task. stop() keeps them allocated so a later
         * start() is cheap, but leaving them resident while the camera is
         * disabled wastes ~13 MB and stresses the WiFi DMA heap. */
        local_camera_deinit();
    }

    return err;
}

esp_err_t local_camera_copy_scaled_rgb565(uint8_t *dst, int dst_w, int dst_h)
{
    if (dst == NULL || dst_w <= 0 || dst_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!local_camera_is_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!frame_ready(LOCAL_CAMERA_FRAME_WAIT_MS)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_cam.latest_frame == NULL || s_cam.out_width == 0 || s_cam.out_height == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cam.frame_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint16_t *src = (const uint16_t *)s_cam.latest_frame;
    uint16_t *out = (uint16_t *)dst;
    for (int y = 0; y < dst_h; y++) {
        const int sy = (int)(((uint64_t)y * s_cam.out_height) / (uint32_t)dst_h);
        const uint16_t *row = src + (size_t)sy * s_cam.out_width;
        for (int x = 0; x < dst_w; x++) {
            const int sx = (int)(((uint64_t)x * s_cam.out_width) / (uint32_t)dst_w);
            out[(size_t)y * dst_w + x] = row[sx];
        }
    }

    xSemaphoreGive(s_cam.frame_mutex);
    return ESP_OK;
}
