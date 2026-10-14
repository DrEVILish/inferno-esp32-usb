// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal: the EMAC IEEE 1588 system time as the device clock. Only the PTP
// task (and mc_start) call these; mc_now_ns() may be called from anywhere.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Capture the nominal addend and start the counter at a sane time
// (esp_timer-based, continuous with the pre-init fallback). Requires
// net_ptp_hw_ready().
void mc_hw_init(void);
bool mc_hw_ready(void);

// Add delta_ns to the counter (absolute re-initialisation, done in a critical
// section) and bump the epoch.
void mc_hw_step(int64_t delta_ns);

// Absolute frequency correction relative to the nominal addend
// (positive = counter runs faster).
void mc_hw_set_freq_ppb(double ppb);
