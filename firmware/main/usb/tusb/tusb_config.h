// SPDX-License-Identifier: GPL-3.0-or-later
//
// TinyUSB configuration for usb-inferno (device only, ESP32-P4 HS OTG = rhport 1).
//
// We use espressif/tinyusb directly (not esp_tinyusb): esp_tinyusb owns the
// descriptor callbacks and caps the string table at 8 entries, but the UAC2
// channel names alone need 64 strings. No built-in class driver is enabled;
// the audio function is our own class driver (usb/uac2.c, registered with
// usbd_app_driver_get_cb()).
//
// This directory is added to the include path of both the tinyusb component
// and main (see main/CMakeLists.txt).
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// CFG_TUSB_MCU (OPT_MCU_ESP32P4) is set by the tinyusb component.
#define CFG_TUSB_OS                 OPT_OS_FREERTOS
#define CFG_TUSB_DEBUG              0

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_HIGH_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64
#define CFG_TUD_TASK_QUEUE_SZ       32

// DWC2: Buffer DMA (the HS core has internal DMA). Slave mode stays compiled
// in as a fallback should GHWCFG2 report no internal DMA.
#define CFG_TUD_DWC2_DMA_ENABLE     1
#define CFG_TUD_DWC2_SLAVE_ENABLE   1

// The P4 reaches internal SRAM through the L1 data cache: TinyUSB cleans IN
// buffers before DMA and invalidates OUT buffers after DMA. Every buffer we
// hand to TinyUSB is therefore cache-line aligned and a whole number of lines.
#define CFG_TUD_MEM_DCACHE_ENABLE       1
#define CFG_TUD_MEM_DCACHE_LINE_SIZE    CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CFG_TUSB_MEM_DCACHE_LINE_SIZE   CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(CONFIG_CACHE_L1_CACHE_LINE_SIZE)))

// No built-in classes.
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_AUDIO               0
#define CFG_TUD_VIDEO               0
#define CFG_TUD_VENDOR              0

#ifdef __cplusplus
}
#endif
