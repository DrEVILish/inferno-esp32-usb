// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal: PTPv1 (IEEE 1588-2002) follower. Never a leader.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "clock/mediaclock.h"

void ptpv1_start(void);
bool ptpv1_locked(void);
int32_t ptpv1_offset_ns(void);
void ptpv1_leader_id(uint8_t id[8]);
void ptpv1_get_stats(mc_stats_t *s);
