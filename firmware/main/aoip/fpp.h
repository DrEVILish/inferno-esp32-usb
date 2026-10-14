// SPDX-License-Identifier: GPL-3.0-or-later
//
// RX flow packet-size (fpp) selection for flow requests.
//
// Pure logic, host-testable: depends only on <stdint.h>.
// Run: gcc -Wall -Wextra tools/fpp_select_test.c -o /tmp/fpp_select_test && /tmp/fpp_select_test
#pragma once

#include <stdint.h>

// Pick the fpp to request from a transmitter whose advertisement allows
// [fpp_min, fpp_max], capped by what fits an unfragmented frame (mtu_cap)
// and by the latency profile (profile_cap: a packet arrives at least fpp
// frames after its timestamp, so fpp must fit inside the AoIP latency).
//
// Requires fpp_min <= mtu_cap: the caller refuses the flow otherwise (such
// packets would fragment and never reach the audio hook, which drops
// fragments; subscribing would mean silence). May return a value above the
// profile cap when the transmitter's minimum demands it: degraded but
// unfragmented, visible in rx_late. May return 0 for a degenerate
// advertisement (caller maps 0 -> 1, as before).
static inline uint32_t aoip_select_fpp(uint32_t fpp_min, uint32_t fpp_max,
                                       uint32_t mtu_cap, uint32_t profile_cap)
{
    uint32_t fpp = fpp_max < mtu_cap ? fpp_max : mtu_cap;
    if (fpp > profile_cap) fpp = profile_cap;
    if (fpp < fpp_min) fpp = fpp_min;
    return fpp;
}
