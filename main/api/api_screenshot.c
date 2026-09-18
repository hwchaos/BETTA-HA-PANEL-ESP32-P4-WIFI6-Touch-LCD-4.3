/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "app_config.h"
#include "diag/display_flash_watch.h"
#include "diag/dsi_underrun_watch.h"
#include "diag/system_log.h"
#include "drivers/display_init.h"
#include "lvgl.h"
#include "util/log_tags.h"

#define SCREENSHOT_LOCK_TIMEOUT_MS 1500U

static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_text_error(httpd_req_t *req, const char *status, const char *message)
{
    set_common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, message);
}

static void write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8) & 0xffU);
    dst[2] = (uint8_t)((value >> 16) & 0xffU);
    dst[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void screenshot_peer_addr(httpd_req_t *req, char *out, size_t out_len)
{
    snprintf(out, out_len, "-");
    if (req == NULL) {
        return;
    }

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd < 0) {
        return;
    }

    struct sockaddr_storage addr = {0};
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_len) != 0) {
        return;
    }

    if (addr.ss_family == AF_INET && addr_len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)&addr;
        const uint8_t *b = (const uint8_t *)&sa->sin_addr.s_addr;
        snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3]);
        return;
    }

    if (addr.ss_family == AF_INET6 && addr_len >= sizeof(struct sockaddr_in6)) {
        const uint8_t *b = (const uint8_t *)&((const struct sockaddr_in6 *)&addr)->sin6_addr;
        const bool mapped = (memcmp(b, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12) == 0);
        if (mapped) {
            snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)b[12], (unsigned)b[13], (unsigned)b[14], (unsigned)b[15]);
        } else if (inet_ntop(AF_INET6, b, out, (socklen_t)out_len) == NULL) {
            snprintf(out, out_len, "-");
        }
    }
}

/* Query flag helper: true when "?name=1" (or "&name=1") is present. */
static bool query_flag(httpd_req_t *req, const char *name)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }

    char value[8];
    if (httpd_query_key_value(query, name, value, sizeof(value)) != ESP_OK) {
        return false;
    }

    return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

static void write_bmp_header(uint8_t *header, uint32_t width, uint32_t height, uint32_t row_stride)
{
    const uint32_t pixel_data_size = row_stride * height;
    memset(header, 0, 54U);
    header[0] = 'B';
    header[1] = 'M';
    write_u32_le(&header[2], 54U + pixel_data_size);
    write_u32_le(&header[10], 54U);
    write_u32_le(&header[14], 40U);
    write_u32_le(&header[18], width);
    write_u32_le(&header[22], height);
    write_u16_le(&header[26], 1U);
    write_u16_le(&header[28], 24U);
    write_u32_le(&header[34], pixel_data_size);
    write_u32_le(&header[38], 2835U);
    write_u32_le(&header[42], 2835U);
}

/* Fast path: copy the panel's own scan-out framebuffer.
 *
 * The previous implementation called lv_snapshot_take() on every request, which
 * allocates a full RGB888 screen buffer (~1.8 MB) and re-renders the whole
 * widget tree.  Any browser tab or monitoring script polling this endpoint
 * therefore saturated PSRAM/DSI bandwidth long enough to underrun the display -
 * the "light blue flash" on screen.  Reading the framebuffer the panel is
 * already scanning out costs one row conversion and no LVGL work at all, so a
 * screenshot can no longer disturb the display. */
static esp_err_t send_framebuffer_bmp(httpd_req_t *req, const uint8_t *fb, size_t fb_bytes, bool swap_channels)
{
    const uint32_t width = APP_SCREEN_WIDTH;
    const uint32_t height = APP_SCREEN_HEIGHT;
    const uint32_t src_stride = width * 2U;

    if (fb == NULL || fb_bytes < (size_t)src_stride * height) {
        return send_text_error(req, "503 Service Unavailable", "Panel framebuffer is not available");
    }

    const uint32_t row_stride = (width * 3U + 3U) & ~3U;
    const uint32_t pad_len = row_stride - width * 3U;
    uint8_t *row = (uint8_t *)heap_caps_malloc(row_stride, MALLOC_CAP_8BIT);
    if (row == NULL) {
        return send_text_error(req, "500 Internal Server Error", "Out of memory");
    }

    uint8_t bmp_header[54];
    write_bmp_header(bmp_header, width, height, row_stride);

    set_common_headers(req);
    httpd_resp_set_type(req, "image/bmp");

    esp_err_t err = httpd_resp_send_chunk(req, (const char *)bmp_header, sizeof(bmp_header));
    if (err != ESP_OK) {
        free(row);
        return err;
    }

    /* BMP rows are stored bottom-up. */
    uint32_t rows_since_yield = 0;
    for (int32_t y = (int32_t)height - 1; y >= 0; y--) {
        const uint16_t *src = (const uint16_t *)(fb + (uint32_t)y * src_stride);
        uint8_t *dst = row;
        for (uint32_t x = 0; x < width; x++) {
            const uint16_t px = src[x];
            /* RGB565 is (red << 11) | (green << 5) | blue and BMP wants blue,
             * green, red; the extra shifts replicate the high bits so the result
             * spans the full 0..255 range. */
            uint8_t blue = (uint8_t)((px & 0x1FU) << 3);
            uint8_t green = (uint8_t)(((px >> 5) & 0x3FU) << 2);
            uint8_t red = (uint8_t)(((px >> 11) & 0x1FU) << 3);
            blue |= (uint8_t)(blue >> 5);
            green |= (uint8_t)(green >> 6);
            red |= (uint8_t)(red >> 5);
            if (swap_channels) {
                const uint8_t tmp = blue;
                blue = red;
                red = tmp;
            }
            dst[0] = blue;
            dst[1] = green;
            dst[2] = red;
            dst += 3;
        }
        for (uint32_t p = 0; p < pad_len; p++) {
            dst[p] = 0;
        }

        err = httpd_resp_send_chunk(req, (const char *)row, row_stride);
        if (err != ESP_OK) {
            free(row);
            return err;
        }

        /* A 1024x600 capture is ~1.8 MB in 3 kB chunks; the socket absorbs most
         * of them without blocking, so without this the handler would own the
         * core for the whole transfer and starve the UI task. */
        if (++rows_since_yield >= APP_HTTP_STREAM_YIELD_UNITS) {
            rows_since_yield = 0;
            vTaskDelay(1);
        }
    }

    err = httpd_resp_send_chunk(req, NULL, 0);
    free(row);
    return err;
}

/* Legacy path, kept behind "?legacy=1" for A/B comparisons: it renders through
 * lv_snapshot_take() and therefore reproduces the PSRAM/DSI load that the fast
 * path avoids. */
static esp_err_t send_snapshot_bmp(httpd_req_t *req)
{
#if !LV_USE_SNAPSHOT
    return send_text_error(req, "501 Not Implemented", "LVGL snapshot support is disabled");
#else
    lv_draw_buf_t *snapshot = NULL;
    if (!display_lock(SCREENSHOT_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG_HTTP, "Screenshot request failed: could not lock display");
        return send_text_error(req, "503 Service Unavailable", "Display is busy");
    }

    snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    display_unlock();

    if (snapshot == NULL) {
        ESP_LOGW(TAG_HTTP, "Screenshot request failed: lv_snapshot_take returned NULL");
        return send_text_error(req, "500 Internal Server Error", "Failed to capture screenshot");
    }

    const uint32_t width = snapshot->header.w;
    const uint32_t height = snapshot->header.h;
    const uint32_t src_row_bytes = width * 3U;
    const uint32_t src_stride = snapshot->header.stride;

    if (width == 0 || height == 0 || snapshot->data == NULL || src_stride < src_row_bytes) {
        lv_draw_buf_destroy(snapshot);
        return send_text_error(req, "500 Internal Server Error", "Invalid screenshot buffer");
    }

    const uint32_t bmp_row_stride = (src_row_bytes + 3U) & ~3U;
    const uint32_t pad_len = bmp_row_stride - src_row_bytes;
    const uint64_t pixel_data_size_64 = (uint64_t)bmp_row_stride * (uint64_t)height;
    const uint64_t file_size_64 = 54ULL + pixel_data_size_64;

    if (pixel_data_size_64 > UINT32_MAX || file_size_64 > UINT32_MAX || width > INT32_MAX || height > INT32_MAX) {
        lv_draw_buf_destroy(snapshot);
        return send_text_error(req, "500 Internal Server Error", "Screenshot is too large");
    }

    uint8_t bmp_header[54];
    write_bmp_header(bmp_header, width, height, bmp_row_stride);

    set_common_headers(req);
    httpd_resp_set_type(req, "image/bmp");

    esp_err_t err = httpd_resp_send_chunk(req, (const char *)bmp_header, sizeof(bmp_header));
    if (err != ESP_OK) {
        lv_draw_buf_destroy(snapshot);
        return err;
    }

    static const uint8_t pad_bytes[3] = {0, 0, 0};
    uint32_t rows_since_yield = 0;
    for (int32_t y = (int32_t)height - 1; y >= 0; y--) {
        const uint8_t *row = snapshot->data + ((uint32_t)y * src_stride);
        err = httpd_resp_send_chunk(req, (const char *)row, src_row_bytes);
        if (err != ESP_OK) {
            lv_draw_buf_destroy(snapshot);
            return err;
        }

        if (pad_len > 0U) {
            err = httpd_resp_send_chunk(req, (const char *)pad_bytes, pad_len);
            if (err != ESP_OK) {
                lv_draw_buf_destroy(snapshot);
                return err;
            }
        }

        if (++rows_since_yield >= APP_HTTP_STREAM_YIELD_UNITS) {
            rows_since_yield = 0;
            vTaskDelay(1);
        }
    }

    err = httpd_resp_send_chunk(req, NULL, 0);
    lv_draw_buf_destroy(snapshot);
    return err;
#endif
}

esp_err_t api_screenshot_bmp_get_handler(httpd_req_t *req)
{
    const bool want_legacy = query_flag(req, "legacy");
    const bool swap_channels = query_flag(req, "swap");
    char peer[24];
    screenshot_peer_addr(req, peer, sizeof(peer));

    /* Every capture is logged with its client and duration: if the display
     * flashes, whoever is polling this endpoint shows up in the log right
     * before the flash. */
    const char *mode = want_legacy ? "legacy" : "framebuffer";
    ESP_LOGI(TAG_HTTP, "Screenshot request from %s (mode=%s)", peer, mode);
    system_log_event("http", "screenshot.bmp from %s (%s)", peer, mode);
    display_flash_watch_note_snapshot(true);

    size_t fb_bytes = 0;
    const uint8_t *fb = want_legacy ? NULL : (const uint8_t *)display_frame_buffer_get(0, &fb_bytes);

    const int64_t started_us = esp_timer_get_time();
    /* Streaming a 1.2 MB framebuffer competes with the display for PSRAM
     * bandwidth, so it is bracketed like the other heavy consumers. */
    dsi_bus_activity_begin(DSI_BUS_SCREENSHOT);
    esp_err_t err;
    if (fb != NULL) {
        err = send_framebuffer_bmp(req, fb, fb_bytes, swap_channels);
    } else {
        if (!want_legacy) {
            ESP_LOGW(TAG_HTTP, "Panel framebuffer unavailable on this variant, using LVGL snapshot");
            system_log_event("http", "screenshot fallback to snapshot (no framebuffer)");
        }
        err = send_snapshot_bmp(req);
    }
    dsi_bus_activity_end(DSI_BUS_SCREENSHOT);
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;

    display_flash_watch_note_snapshot(false);
    system_log_event("http", "screenshot %s done in %lldms", mode, (long long)elapsed_ms);
    ESP_LOGI(TAG_HTTP, "Screenshot from %s (%s) finished in %lld ms (%s)",
             peer, mode, (long long)elapsed_ms, esp_err_to_name(err));
    return err;
}
