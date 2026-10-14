// SPDX-License-Identifier: GPL-3.0-or-later
//
// Histograms behind sys/perf.h. Deliberately FreeRTOS-free: the recording
// inline functions live in perf.h (IRAM-safe); only the telemetry-side
// report lives here, so the file stays host-testable (tools/perf_host_test.c).
#include "sys/perf.h"

perf_hist_t s_perf_hist[PERF_N];
uint32_t s_perf_start[2][PERF_N];

static const char *s_names[PERF_N] = {
    "rx_input", "rx_hook", "tx_build", "tx_send", "usb_sof", "usb_xfer",
};

const char *perf_name(perf_probe_t p) { return s_names[p]; }

void perf_report(perf_probe_t p, perf_window_t *out)
{
    perf_hist_t *h = &s_perf_hist[p];
    const uint32_t n = atomic_exchange_explicit(&h->n, 0, memory_order_relaxed);
    const uint32_t max = atomic_exchange_explicit(&h->max, 0, memory_order_relaxed);
    uint32_t b[33], sum = 0;
    for (unsigned i = 0; i < 33; i++) {
        b[i] = atomic_exchange_explicit(&h->bucket[i], 0, memory_order_relaxed);
        sum += b[i];
    }
    out->n = n;
    out->max = max;
    out->overflow = b[32];
    if (!sum) {
        out->p50 = out->p99 = 0;
        return;
    }
    // Percentiles from the histogram: bucket i holds durations [8i, 8i+8);
    // report its centre (the overflow bucket sits at 260). First bucket
    // whose cumulative count reaches the rank wins.
    const uint32_t rank50 = (sum + 1u) / 2u, rank99 = sum - sum / 100u;
    uint32_t cum = 0;
    bool have50 = false;
    out->p50 = out->p99 = 0;
    for (unsigned i = 0; i < 33; i++) {
        cum += b[i];
        const uint32_t centre = i == 32 ? 260u : i * 8u + 4u;
        if (!have50 && cum >= rank50) {
            out->p50 = centre;
            have50 = true;
        }
        if (cum >= rank99) {
            out->p99 = centre;
            break;
        }
    }
}
