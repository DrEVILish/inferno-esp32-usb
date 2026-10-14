// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AoIP device (Inferno-compatible): discovery (mDNS), ARC control
// server, CMC, info/heartbeat multicasts, flow-control server (receivers
// request our TX flows), TX packetizer (from g_ring_usb_to_net), channel
// subscriber (requests flows from transmitters, persisted in NVS) and RX
// decode (into g_ring_net_to_usb).
//
// Identity: name UI_NAME_PREFIX + last 3 MAC bytes, 32 TX ch "01".."32",
// 32 RX ch "01".."32", 48 kHz, 24-bit, latency UI_NET_LATENCY_NS.
//
// Owner: aoip/ (AoIP agent).
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "app_config.h"

// Start everything. Call after net_start() and mc_start(). Services come up
// when link/IP appear and re-announce on IP changes.
//
// CONFIG_USBINF_TEST_NET_LOOPBACK: TX channel N carries RX channel N, read
// from g_ring_net_to_usb at (TX frame - UI_NET_LATENCY_FRAMES) without
// clearing. In that build nothing else may consume g_ring_net_to_usb
// (the USB IN path must not read-and-clear it).
esp_err_t aoip_start(void);

typedef struct {
    uint32_t tx_flows, rx_flows;
    uint32_t tx_packets, rx_packets;
    uint32_t rx_late, rx_early, rx_seq_gaps;
    uint32_t subscriptions_total, subscriptions_ok;
    // Added by the AoIP implementation (additive):
    uint32_t tx_send_fail;      // net_udp_send_raw failures / unresolved destination
    uint32_t tx_underruns;      // TX packets sent as silence (TX ring not valid)
    uint32_t tx_lag_resets;     // TX timeline re-anchored (lag, clock jump, epoch)
    uint32_t rx_wrong_len;      // RX packets whose length is not a whole frame count
    uint32_t rx_ka_sent, rx_ka_fail;        // RX flow keepalives (13 37)
    uint32_t fc_requests, fc_failures;      // flow-control requests we sent
    uint32_t arc_requests;
    uint32_t nvs_writes;            // NVS commits that changed flash
    uint32_t nvs_skipped;           // quiet periods with nothing new to write
    // latency windows since the previous aoip_get_stats() (frames;
    // INT32_MAX / INT32_MIN = no sample), see docs/14-latency.md
    int32_t  rx_margin_min;                 // all RX flows: ts + latency - arrival
    int32_t  rx_slot_margin_min[UI_RX_MAX_FLOWS];   // per RX flow slot
    int32_t  tx_lag_max;                    // send time - (ts + fpp)
} aoip_stats_t;
void aoip_get_stats(aoip_stats_t *s);
