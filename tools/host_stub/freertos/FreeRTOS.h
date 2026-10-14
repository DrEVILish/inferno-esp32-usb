// Host-test stub for freertos/FreeRTOS.h.
#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
// Host-test stub additions (subscriber.c / aoip.c syntax checks).
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS 1
#define pdFAIL 0
