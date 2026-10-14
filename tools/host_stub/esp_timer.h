// Host-test stub for esp_timer/esp_timer.h equivalents used by aoip.c.
#pragma once

#include <stdint.h>

static inline int64_t esp_timer_get_time(void) { return 0; }
