// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cycle-counter probes for the hot paths (docs/15-profile.md).
//
// perf_begin()/perf_end() bracket a region; the duration lands in an
// 8-cycle histogram (32 buckets for 0-255 cycles plus an overflow bucket).
// They are static inline and IRAM-safe, so the USB ISR can use them
// (check_iram.py walks them like every other ISR-path function).
//
// Rules: begin and end must be called from the same core, a probe must not
// nest within itself (TX_SEND inside TX_BUILD is fine: different probes),
// and each probe ID is used from one execution context only. The recording
// costs ~10 cycles per pair and is always compiled in; docs/15 documents
// the expected overhead (~1-3 % of one core at the profile-2 packet rates).
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"
#include "esp_cpu.h"

#include "app_config.h"     // UI_PERF_PROBES

typedef enum {
    PERF_RX_INPUT = 0,  // net.c rx_input: whole EMAC RX fast path per frame
    PERF_RX_HOOK,       // flows_rx.c rx_hook: audio decode per audio packet
    PERF_TX_BUILD,      // flows_tx.c build_and_send per TX packet
    PERF_TX_SEND,       // net_udp_send_raw inside build_and_send (nested)
    PERF_USB_SOF,       // uac2_sof_isr per microframe
    PERF_USB_XFER,      // uac2_xfer_isr per transfer completion
    PERF_N
} perf_probe_t;

typedef struct {
    uint32_t n;         // samples recorded in this window
    uint32_t p50, p99;  // cycles, 8-cycle bucket resolution
    uint32_t max;       // exact maximum, cycles
    uint32_t overflow;  // samples above 255 cycles
} perf_window_t;

// Windowed report since the previous call for this probe (and reset).
// Runs in the telemetry task; recording may continue concurrently — losing
// one sample per read is accepted (telemetry only).
void perf_report(perf_probe_t p, perf_window_t *out);
const char *perf_name(perf_probe_t p);

// Shared probe state (perf.c). The histograms are relaxed atomics so both
// cores record; the start slots are indexed per core, and each probe ID is
// single-context, so plain slots are safe (no cross-core sharing).
// UI_PERF_PROBES = 0 (Kconfig USBINF_PERF_PROBES off) compiles the begin/
// end pair down to no-ops; the telemetry t_* keys then never appear
// (perf_report() sees an empty histogram) and nothing else changes.
typedef struct {
    _Atomic uint32_t bucket[33];    // [i] = durations in [8i, 8i+8), [32] = overflow
    _Atomic uint32_t max;           // exact max, cycles
    _Atomic uint32_t n;
} perf_hist_t;
extern perf_hist_t s_perf_hist[PERF_N];
extern uint32_t s_perf_start[2][PERF_N];    // [core][probe], 0 = not bracketing

static inline uint32_t perf_cycle_now(void) { return esp_cpu_get_cycle_count(); }

#if UI_PERF_PROBES
static inline void perf_begin(perf_probe_t p)
{
    s_perf_start[esp_cpu_get_core_id()][p] = perf_cycle_now();
}

static inline void perf_end(perf_probe_t p)
{
    const unsigned core = (unsigned)esp_cpu_get_core_id();
    const uint32_t s = s_perf_start[core][p];
    if (!s) return;                             // end without begin: skip
    s_perf_start[core][p] = 0;
    const uint32_t d = perf_cycle_now() - s;    // unsigned: cycle-counter wrap is fine
    perf_hist_t *h = &s_perf_hist[p];
    uint32_t i = d >> 3;
    if (i > 32) i = 32;
    atomic_fetch_add_explicit(&h->bucket[i], 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->n, 1u, memory_order_relaxed);
    uint32_t m = atomic_load_explicit(&h->max, memory_order_relaxed);
    while (d > m &&
           !atomic_compare_exchange_weak_explicit(&h->max, &m, d,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {}
}
#else
static inline void perf_begin(perf_probe_t p) { (void)p; }
static inline void perf_end(perf_probe_t p) { (void)p; }
#endif
