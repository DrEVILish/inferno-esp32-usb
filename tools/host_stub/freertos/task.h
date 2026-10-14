// Host-test stub for freertos/task.h (subscriber/aoip syntax checks only).
#pragma once

#include "freertos/FreeRTOS.h"

static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }
static inline void vTaskDelete(void *t) { (void)t; }
static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack,
                                                 void *arg, int prio, void **handle, int core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)core;
    if (handle) *handle = 0;
    return pdPASS;
}
static inline void *xTaskGetCurrentTaskHandle(void) { return 0; }
static inline int xTaskNotify(void *t, uint32_t v, int action) { (void)t; (void)v; (void)action; return 1; }
static inline int xTaskNotifyWait(uint32_t clear, uint32_t mask, uint32_t *v, TickType_t t)
{
    (void)clear; (void)mask; (void)v; (void)t; return 0;
}
static inline void xTaskNotifyStateClear(void *t) { (void)t; }
