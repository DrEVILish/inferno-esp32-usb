// Host-test stubs for ESP-IDF headers (no IDF needed).
// Used by tools/arc_info_host_test.c via -Itools/host_stub.
#pragma once

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_INVALID_STATE 5
#define ESP_ERR_NOT_FINISHED 6
#define ESP_ERR_INVALID_SIZE 7
static inline const char *esp_err_to_name(int e) { (void)e; return "ERR"; }
