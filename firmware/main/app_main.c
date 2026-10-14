// SPDX-License-Identifier: GPL-3.0-or-later
//
// usb-inferno: fixed 32x32 AoIP <-> USB Audio Class 2.0 bridge.
// Boots straight into operation; there is nothing to configure.
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"

#include "app_config.h"
#include "audio/ring.h"
#include "clock/mediaclock.h"
#include "net/net.h"
#include "usb/uac2.h"
#include "aoip/aoip.h"
#include "sys/telem.h"

static const char *TAG = "app";

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s %s, %u Hz, %ux%u", UI_MODEL_NAME, UI_FW_VERSION,
             UI_SAMPLE_RATE, UI_CHANNELS, UI_CHANNELS);

    nvs_init();
    ring_init();

    // Network first: the media clock is the EMAC's 1588 counter.
    ESP_ERROR_CHECK(net_start());
    mc_start();

    // USB runs from the media clock whether or not PTP is locked.
    ESP_ERROR_CHECK(uac2_start());

    // In the USB loopback test build the AoIP side still runs (discovery,
    // PTP); only the audio routing in uac2.c changes.
    ESP_ERROR_CHECK(aoip_start());
    ESP_ERROR_CHECK(telem_start());

    // Internal RAM is the tight resource (docs/01-architecture.md §8).
    ESP_LOGI(TAG, "running; internal heap free %u (min %u), PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
