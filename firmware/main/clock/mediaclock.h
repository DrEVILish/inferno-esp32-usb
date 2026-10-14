// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one time base of the device: the EMAC IEEE 1588 system time, slaved to
// the network's PTPv1 leader by the servo in ptpv1.c (addend trim, so the
// hardware counter *is* PTP time). Before lock it free-runs on the board
// oscillator, so everything keeps working without a network.
//
// Owner: clock/ (net+clock agent).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

// Start the 1588 clock (requires net_start() to have brought up the EMAC) and
// the PTPv1 follower. Idempotent.
void mc_start(void);

// PTP time in ns. Safe from ISRs and any core (IRAM). Monotonic except for
// deliberate steps (see mc_epoch()).
uint64_t mc_now_ns(void);

// Media clock in frames at UI_SAMPLE_RATE: mc_now_ns() * fs / 1e9.
uint64_t mc_now_frames(void);

// Convert between ns and frames (exact integer maths, no drift).
uint64_t mc_ns_to_frames(uint64_t ns);
uint64_t mc_frames_to_ns(uint64_t frames);

// True once the servo has locked to a PTP leader (offset within limits).
bool mc_locked(void);

// mc_now_ns() and mc_epoch() read together, consistently: the time is on the
// timescale of the returned epoch even if a step runs concurrently on the
// other core (the step is bracketed by a sequence counter; a reader retries
// while it is in progress, at most a few µs). Safe from ISRs (IRAM). Audio
// paths that re-anchor on an epoch change must use this, not two separate
// calls: time-then-epoch can pair an old-timescale time with the new epoch.
uint64_t mc_now_ns_epoch(uint32_t *epoch);

// Incremented every time the clock is stepped (initial lock, leader change,
// large offset). Audio paths re-anchor their positions when it changes.
uint32_t mc_epoch(void);

// Current frequency correction applied to the 1588 counter, parts per billion
// (positive = local oscillator slow, counter sped up).
int32_t mc_freq_ppb(void);

// Last measured offset from the leader in ns (telemetry).
int32_t mc_offset_ns(void);

// Leader identity (zeroes if none): bytes 0..5 = the leader's PTPv1
// sourceUuid (its MAC), bytes 6..7 = its sourcePortId, big-endian. Derive an
// EUI-64 as id[0..2] FF FE id[3..5] where one is needed.
void mc_leader_id(uint8_t id[8]);

// Detailed PTP counters for telemetry (additive; everything is cumulative
// since boot except the state fields).
typedef struct {
    uint32_t syncs, follow_ups, delay_reqs, delay_resps;
    uint32_t no_hw_rx_ts, no_hw_tx_ts, queue_drops;
    uint32_t steps, lock_losses, outliers, phase_shifts, leader_changes;
    uint32_t delay_samples, delay_rejected;
    int32_t  path_delay_ns;     // filtered leader-to-us delay (0 until known)
    uint8_t  stage;             // 0 no leader/idle, 1 frequency estimate, 2 tracking
} mc_stats_t;
void mc_get_stats(mc_stats_t *s);
