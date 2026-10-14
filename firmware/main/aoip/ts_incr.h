// SPDX-License-Identifier: GPL-3.0-or-later
//
// Incremental packet-timestamp split: the AoIP media header carries the
// packet's first frame index as (u32 seconds, u32 sub-second samples).
// Deriving it per packet costs two 64-bit divisions (ts / rate, ts % rate);
// the packetizer advances next_ts by fpp frames at a time, so the split can
// advance with one conditional carry instead.
//
// Pure logic, host-testable: depends only on <stdint.h> and UI_SAMPLE_RATE
// from app_config.h (which falls back to defaults without sdkconfig.h).
// Test: tools/ts_bitmap_test.c
#pragma once

#include <stdint.h>

#include "app_config.h"

typedef struct {
    uint32_t sec;   // whole seconds
    uint32_t sub;   // samples into the second, < UI_SAMPLE_RATE
} ts_split_t;

// Recompute the split from a frame index (bootstrap / re-anchor: rare).
static inline void ts_split_load(ts_split_t *t, uint64_t ts)
{
    t->sec = (uint32_t)(ts / UI_SAMPLE_RATE);
    t->sub = (uint32_t)(ts % UI_SAMPLE_RATE);
}

// Advance by fpp frames after a packet carrying this split went out.
// fpp < UI_SAMPLE_RATE is required (the flow checks enforce fpp <= 256).
static inline void ts_split_advance(ts_split_t *t, uint32_t fpp)
{
    t->sub += fpp;
    if (t->sub >= UI_SAMPLE_RATE) {
        t->sub -= UI_SAMPLE_RATE;
        t->sec++;
    }
}
