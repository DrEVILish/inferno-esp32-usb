// SPDX-License-Identifier: GPL-3.0-or-later
//
// Telemetry: any UDP datagram to UI_TELEM_PORT gets back one "key=value\n"
// snapshot of every module's counters. Never log from audio paths; read this.
#pragma once
#include "esp_err.h"
esp_err_t telem_start(void);
