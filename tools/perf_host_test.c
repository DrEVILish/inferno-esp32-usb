// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for sys/perf.c histogram + percentile math (no IDF needed):
//   tools/host_tests.sh  (or: gcc -DUAC2_HOST_SIM -I firmware/main
//                         -I tools/host_stub tools/perf_host_test.c
//                         firmware/main/sys/perf.c)
#include <stdio.h>

#include "sys/perf.h"

uint32_t host_fake_cycles;

static int failures;

static void expect(const char *what, unsigned long got, unsigned long want)
{
    if (got != want) {
        printf("FAIL %s: got %lu want %lu\n", what, got, want);
        failures++;
    }
}

static void record(perf_probe_t p, uint32_t start, uint32_t dur)
{
    host_fake_cycles = start;
    perf_begin(p);
    host_fake_cycles = start + dur;
    perf_end(p);
}

int main(void)
{
    perf_window_t w;

    // empty window: n = 0
    perf_report(PERF_RX_HOOK, &w);
    expect("empty n", w.n, 0);

    // one sample, 100 cycles: p50 = p99 = bucket centre (100>>3=12 -> 100)
    record(PERF_RX_HOOK, 1000, 100);
    perf_report(PERF_RX_HOOK, &w);
    expect("n=1", w.n, 1);
    expect("p50(100c)", w.p50, 100);
    expect("p99(100c)", w.p99, 100);
    expect("max(100c)", w.max, 100);

    // distribution: 9x 8 cycles, 1x 200 cycles -> p50 = 12 (8>>3=1 -> 12)
    // p99 = rank 9.9 -> first cum >= 10 -> the 200 bucket (200>>3=25 -> 204)
    for (int i = 0; i < 9; i++) record(PERF_RX_HOOK, 2000, 8);
    record(PERF_RX_HOOK, 2000, 200);
    perf_report(PERF_RX_HOOK, &w);
    expect("n=10", w.n, 10);
    expect("p50 mixed", w.p50, 12);
    expect("p99 mixed", w.p99, 204);
    expect("max mixed", w.max, 200);

    // overflow: 300 cycles lands in the overflow bucket, max exact
    record(PERF_USB_SOF, 5000, 300);
    perf_report(PERF_USB_SOF, &w);
    expect("ovf n", w.n, 1);
    expect("ovf count", w.overflow, 1);
    expect("ovf max", w.max, 300);
    expect("ovf p50", w.p50, 260);

    // cycle-counter wrap: start 0xFFFFFFF0, dur 32 -> end wraps to 0x10;
    // unsigned subtraction must yield exactly 32
    host_fake_cycles = 0xFFFFFFF0u;
    perf_begin(PERF_TX_BUILD);
    host_fake_cycles = 0x10u;                   // wrapped past zero
    perf_end(PERF_TX_BUILD);
    perf_report(PERF_TX_BUILD, &w);
    expect("wrap n", w.n, 1);
    expect("wrap max", w.max, 32);
    expect("wrap p50", w.p50, 36);

    // end without begin is a no-op
    perf_end(PERF_TX_SEND);
    perf_report(PERF_TX_SEND, &w);
    expect("orphan end n", w.n, 0);

    if (failures == 0) printf("perf_host_test: all pass\n");
    return failures != 0;
}
