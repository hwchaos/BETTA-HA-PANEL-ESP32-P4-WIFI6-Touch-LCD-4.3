#pragma once

/* HLK-LD2410 24 GHz presence radar on a UART.
 *
 * The sensor streams report frames (F4 F3 F2 F1 .. F8 F7 F6 F5) several times a
 * second; they tell whether a moving and/or a stationary target is present and
 * how far away. The panel uses them to wake the display when someone comes
 * close, and shows the live values on the editor's status page. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "settings/runtime_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LD2410_TARGET_NONE = 0,
    LD2410_TARGET_MOVING = 1,
    LD2410_TARGET_STATIC = 2,
    LD2410_TARGET_BOTH = 3,
} ld2410_target_t;

typedef struct {
    bool enabled;
    bool running;          /* UART driver installed */
    bool detected;         /* valid frames in the last seconds */
    char error[64];
    uint32_t frames;       /* valid frames since start */
    uint32_t bad_frames;   /* framing / length errors */
    uint32_t rx_bytes;     /* raw bytes received */
    uint8_t first_bytes[16]; /* first bytes seen (diagnostics) */
    uint8_t first_count;
    uint32_t last_frame_age_ms;
    ld2410_target_t target;
    uint16_t moving_cm;
    uint8_t moving_energy;
    uint16_t static_cm;
    uint8_t static_energy;
    uint16_t detect_cm;
    bool near;             /* a target within the wake distance */
} ld2410_status_t;

/* (Re)starts or stops the radar according to the settings. */
void ld2410_apply_settings(const runtime_settings_t *settings);

void ld2410_get_status(ld2410_status_t *out);

/* Self-test with the UART in internal loopback: sends a synthetic report
 * frame to itself and checks that it is received and parsed. Needs the radar
 * to be running. Returns ESP_OK when the frame came back. */
esp_err_t ld2410_self_test(char *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif
