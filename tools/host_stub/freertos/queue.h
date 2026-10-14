// Host-test stub for freertos/queue.h.
#pragma once

#include "freertos/FreeRTOS.h"

static inline QueueHandle_t xQueueCreate(int n, int sz)
{
    (void)n;
    (void)sz;
    return (QueueHandle_t)1;
}

static inline int xQueueSend(QueueHandle_t q, const void *item, unsigned wait)
{
    (void)q;
    (void)item;
    (void)wait;
    return pdTRUE;
}

static inline int xQueueReceive(QueueHandle_t q, void *item, unsigned wait)
{
    (void)q;
    (void)item;
    (void)wait;
    return pdFALSE;
}
