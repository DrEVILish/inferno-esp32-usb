// Host-test stub for freertos/semphr.h (subscriber/aoip syntax checks only).
#pragma once

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return 0; }
static inline void xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; }
static inline void xSemaphoreGiveRecursive(SemaphoreHandle_t s) { (void)s; }
