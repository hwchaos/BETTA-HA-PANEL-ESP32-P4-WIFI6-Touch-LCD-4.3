/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Network liveness watchdog.
 *
 * Wi-Fi on the P4 panels runs over an ESP32-C6 through ESP-Hosted.  When that
 * transport dies the driver keeps reporting "connected" (the flag comes from
 * the last IP_EVENT_STA_GOT_IP and no disconnect event is ever delivered), so
 * neither the Wi-Fi reconnect ladder nor the HA client escalation fires: the
 * panel stays powered, keeps drawing its UI and answering nothing on the LAN.
 *
 * This watchdog closes that hole.  It probes the LAN from the panel itself and
 * escalates step by step when the whole network is unreachable:
 *   3 dead rounds  -> wifi_mgr_force_reconnect()
 *   6 dead rounds  -> wifi_mgr_force_transport_recover()  (C6 hard recover)
 *   10 dead rounds -> esp_restart()
 *
 * Escalation only happens while the Wi-Fi driver still claims to be connected,
 * so a real router outage (where the driver reports the drop and its own ladder
 * is in charge) never causes a reboot loop. */

typedef struct {
    uint32_t probe_rounds;      /* probe rounds finished since boot */
    uint32_t ok_rounds;         /* rounds with at least one reachable anchor */
    uint32_t fail_rounds;       /* rounds where every anchor was unreachable */
    uint32_t reconnect_count;   /* wifi_mgr_force_reconnect() escalations */
    uint32_t recover_count;     /* C6/hosted hard-recover escalations */
    uint32_t restart_count;     /* esp_restart() escalations (this boot only) */
    uint8_t fail_streak;        /* consecutive dead rounds right now */
    uint8_t max_fail_streak;    /* worst streak since boot */
    int64_t last_ok_uptime_ms;  /* uptime of the last healthy round, -1 if none */
    uint32_t last_probe_ms;     /* duration of the last probe round */
    char gateway[20];           /* probed default gateway, "" when unknown */
    char ha_host[64];           /* probed HA host, "" when not configured */
    uint16_t ha_port;
} net_health_stats_t;

/* One-off probe for /api/diagnostics?net_probe=<host>[:<port>].  It answers the
 * question the counters cannot: right now, what can the panel actually reach?
 * (Which separates "the panel's network is dead" from "Home Assistant is down".) */
typedef struct {
    char host[64];          /* requested host, "" when only the anchors were probed */
    uint16_t port;
    bool ping_ok;           /* ICMP echo answered by the requested host */
    bool tcp_ok;            /* TCP connect to the requested host completed */
    bool gateway_ping_ok;   /* ICMP echo answered by the default gateway */
    bool ha_tcp_ok;         /* TCP connect to the Home Assistant server completed */
    uint32_t elapsed_ms;    /* duration of the whole on-demand probe */
} net_health_probe_result_t;

/* Starts the watchdog task.  ha_ws_url may be NULL/empty: the gateway alone is
 * then used as the anchor.  Safe to call once from app_main(). */
esp_err_t net_health_start(const char *ha_ws_url);

void net_health_get_stats(net_health_stats_t *out);

/* host may be NULL to probe the configured anchors only. */
void net_health_probe_once(const char *host, uint16_t port, net_health_probe_result_t *out);
