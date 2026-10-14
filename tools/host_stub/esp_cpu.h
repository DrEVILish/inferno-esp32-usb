// Host-test stub for esp_cpu.h: fake, controllable cycle counter.
#pragma once

#include <stdint.h>

extern uint32_t host_fake_cycles;

static inline uint32_t esp_cpu_get_cycle_count(void) { return host_fake_cycles; }
static inline int esp_cpu_get_core_id(void) { return 0; }
