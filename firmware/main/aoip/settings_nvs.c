// SPDX-License-Identifier: GPL-3.0-or-later
//
// Persistence of what the controller may change: device name, TX/RX channel
// names, RX subscriptions and TX multicast flows. Replaces Inferno's
// state_storage.rs (TOML files) + saved_settings.rs with NVS blobs.
//
// NVS namespace "aoip", all fixed-size records (version key "ver" = 1):
//   "devname" string   friendly device name (absent = default from the MAC)
//   "txnames" blob     32 x 32 B friendly TX channel names ("" = factory name)
//   "rxnames" blob     32 x 32 B friendly RX channel names
//   "subs"    blob     32 x saved_sub_t (tx channel name, tx device name)
//   "txmc"    blob     UI_TX_MAX_FLOWS x saved_txmc_t (multicast TX flows)
//
// Write policy (docs/13-review-2026-10-13.md, A3): every change only marks
// the affected record dirty. The control task writes once no further change
// has arrived for SETTINGS_QUIET_MS (2 s), so a burst of controller actions
// (a 32-channel patch, a rename sweep) costs one flash write, not dozens.
// Each record is serialised and compared with what is known to be in flash
// (a shadow copy, initialised from the loaded state); only records whose
// bytes differ are written, and nothing is committed if none differ.
// nvs_writes counts commits that changed flash, nvs_skipped flushes that
// found nothing to write. On an NVS error the dirty bits are kept and the
// write is retried after the next quiet period.
//
// A flash write stalls every flash-resident task on both cores for its
// duration (ms; an erase during NVS page GC tens of ms). The USB interrupt
// path is IRAM-only (A2, tools/check_iram.py) and keeps running, so USB
// audio carries on bit-exact; the network side (EMAC RX task, packetizer,
// lwIP) is flash-resident and pauses: RX frames queue in the EMAC DMA ring,
// TX packets due during the stall are sent late or re-anchored. Coalescing
// makes that a rare, controller-initiated event.
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

#include "aoip/aoip_internal.h"

static const char *TAG = "aoip_nvs";
static const char *NS = "aoip";
#define SETTINGS_VERSION 1u

#define SETTINGS_QUIET_MS 2000u

static saved_sub_t s_subs[UI_CHANNELS];
static saved_txmc_t s_txmc[AOIP_TX_FLOW_SLOTS];
static uint32_t s_dirty;
static uint32_t s_dirty_since;

// What is known to be in flash, in serialised form ("absent" == all zero /
// default, which is what a factory-state device would serialise to).
typedef char names_blob_t[UI_CHANNELS][AOIP_NAME_LEN];
static struct {
    bool         ver_ok;
    char         devname[AOIP_NAME_LEN];    // "" = key absent (default name)
    names_blob_t tx, rx;
    saved_sub_t  subs[UI_CHANNELS];
    saved_txmc_t txmc[AOIP_TX_FLOW_SLOTS];
} s_flash;
static uint32_t s_skipped;

static void load_names(nvs_handle_t h, const char *key, char names[UI_CHANNELS][AOIP_NAME_LEN],
                       names_blob_t shadow)
{
    static char buf[UI_CHANNELS][AOIP_NAME_LEN];
    size_t len = sizeof buf;
    if (nvs_get_blob(h, key, buf, &len) != ESP_OK || len != sizeof buf) return;
    memcpy(shadow, buf, sizeof buf);
    for (unsigned i = 0; i < UI_CHANNELS; i++) {
        buf[i][AOIP_NAME_LEN - 1] = 0;
        if (buf[i][0]) strcpy(names[i], buf[i]);
    }
}

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved state (first boot)");
        return;
    }
    uint32_t ver = 0;
    if (nvs_get_u32(h, "ver", &ver) != ESP_OK || ver != SETTINGS_VERSION) {
        ESP_LOGW(TAG, "saved state version %lu unsupported, ignoring", (unsigned long)ver);
        nvs_close(h);
        return;
    }
    s_flash.ver_ok = true;
    char name[AOIP_NAME_LEN];
    size_t len = sizeof name;
    if (nvs_get_str(h, "devname", name, &len) == ESP_OK && name[0]) {
        strcpy(g_aoip.friendly_hostname, name);
        strcpy(s_flash.devname, name);
    }
    load_names(h, "txnames", g_aoip.tx_names, s_flash.tx);
    load_names(h, "rxnames", g_aoip.rx_names, s_flash.rx);
    len = sizeof s_subs;
    if (nvs_get_blob(h, "subs", s_subs, &len) != ESP_OK || len != sizeof s_subs) {
        memset(s_subs, 0, sizeof s_subs);
    }
    for (unsigned i = 0; i < UI_CHANNELS; i++) {
        s_subs[i].txch[AOIP_NAME_LEN - 1] = 0;
        s_subs[i].host[AOIP_NAME_LEN - 1] = 0;
    }
    memcpy(s_flash.subs, s_subs, sizeof s_subs);
    len = sizeof s_txmc;
    if (nvs_get_blob(h, "txmc", s_txmc, &len) != ESP_OK || len != sizeof s_txmc) {
        memset(s_txmc, 0, sizeof s_txmc);
    }
    memcpy(s_flash.txmc, s_txmc, sizeof s_txmc);
    nvs_close(h);
}

void settings_get_subs(saved_sub_t out[UI_CHANNELS])
{
    aoip_lock();
    memcpy(out, s_subs, sizeof s_subs);
    aoip_unlock();
}

void settings_get_txmc(saved_txmc_t out[AOIP_TX_FLOW_SLOTS])
{
    aoip_lock();
    memcpy(out, s_txmc, sizeof s_txmc);
    aoip_unlock();
}

void settings_set_subs(const saved_sub_t subs[UI_CHANNELS])
{
    aoip_lock();
    if (memcmp(s_subs, subs, sizeof s_subs) != 0) {
        memcpy(s_subs, subs, sizeof s_subs);
        settings_mark_dirty(SET_DIRTY_SUBS);
    }
    aoip_unlock();
}

void settings_set_txmc(const saved_txmc_t mc[AOIP_TX_FLOW_SLOTS])
{
    aoip_lock();
    if (memcmp(s_txmc, mc, sizeof s_txmc) != 0) {
        memcpy(s_txmc, mc, sizeof s_txmc);
        settings_mark_dirty(SET_DIRTY_TXMC);
    }
    aoip_unlock();
}

void settings_mark_dirty(uint32_t bits)
{
    aoip_lock();
    s_dirty |= bits;
    s_dirty_since = aoip_now_ms();
    aoip_unlock();
}

// Friendly names as stored: "" where the name is the factory one.
static void serialise_names(char names[UI_CHANNELS][AOIP_NAME_LEN], names_blob_t out)
{
    memset(out, 0, sizeof(names_blob_t));
    for (unsigned i = 0; i < UI_CHANNELS; i++) {
        char fac[4];
        aoip_factory_ch_name(fac, i);
        if (strcmp(names[i], fac) != 0) strcpy(out[i], names[i]);
    }
}

// Write one blob if it differs from the flash shadow. Returns the number of
// records written (0/1), or -1 on error.
static int put_blob(nvs_handle_t h, const char *key, const void *cur, void *shadow, size_t len)
{
    if (memcmp(cur, shadow, len) == 0) return 0;
    if (nvs_set_blob(h, key, cur, len) != ESP_OK) return -1;
    return 1;
}

void settings_flush_if_due(void)
{
    aoip_lock();
    if (!s_dirty || aoip_now_ms() - s_dirty_since < SETTINGS_QUIET_MS) {
        aoip_unlock();
        return;
    }
    const uint32_t dirty = s_dirty;
    s_dirty = 0;

    // Anything actually different from flash?
    const char *devname = strcmp(g_aoip.friendly_hostname, g_aoip.default_hostname) != 0
                        ? g_aoip.friendly_hostname : "";
    bool need = !s_flash.ver_ok;
    static names_blob_t tx, rx;
    if (dirty & SET_DIRTY_NAMES) {
        serialise_names(g_aoip.tx_names, tx);
        serialise_names(g_aoip.rx_names, rx);
        need |= strcmp(devname, s_flash.devname) != 0 ||
                memcmp(tx, s_flash.tx, sizeof tx) != 0 || memcmp(rx, s_flash.rx, sizeof rx) != 0;
    }
    if (dirty & SET_DIRTY_SUBS) need |= memcmp(s_subs, s_flash.subs, sizeof s_subs) != 0;
    if (dirty & SET_DIRTY_TXMC) need |= memcmp(s_txmc, s_flash.txmc, sizeof s_txmc) != 0;
    if (!need) {
        s_skipped++;
        aoip_unlock();
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        goto retry;
    }
    if (!s_flash.ver_ok) {
        if ((err = nvs_set_u32(h, "ver", SETTINGS_VERSION)) != ESP_OK) goto fail;
    }
    if (dirty & SET_DIRTY_NAMES) {
        if (strcmp(devname, s_flash.devname) != 0) {
            err = devname[0] ? nvs_set_str(h, "devname", devname) : nvs_erase_key(h, "devname");
            if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) goto fail;
        }
        if ((put_blob(h, "txnames", tx, s_flash.tx, sizeof tx)) < 0) { err = ESP_FAIL; goto fail; }
        if ((put_blob(h, "rxnames", rx, s_flash.rx, sizeof rx)) < 0) { err = ESP_FAIL; goto fail; }
    }
    if ((dirty & SET_DIRTY_SUBS) && put_blob(h, "subs", s_subs, s_flash.subs, sizeof s_subs) < 0) { err = ESP_FAIL; goto fail; }
    if ((dirty & SET_DIRTY_TXMC) && put_blob(h, "txmc", s_txmc, s_flash.txmc, sizeof s_txmc) < 0) { err = ESP_FAIL; goto fail; }
    err = nvs_commit(h);
    if (err != ESP_OK) goto fail;
    nvs_close(h);

    // Flash now holds the current state.
    s_flash.ver_ok = true;
    if (dirty & SET_DIRTY_NAMES) {
        strcpy(s_flash.devname, devname);
        memcpy(s_flash.tx, tx, sizeof tx);
        memcpy(s_flash.rx, rx, sizeof rx);
    }
    if (dirty & SET_DIRTY_SUBS) memcpy(s_flash.subs, s_subs, sizeof s_subs);
    if (dirty & SET_DIRTY_TXMC) memcpy(s_flash.txmc, s_txmc, sizeof s_txmc);
    CTR_INC(nvs_writes);
    aoip_unlock();
    return;

fail:
    ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
    nvs_close(h);
retry:
    // keep the changes; try again after another quiet period
    s_dirty |= dirty;
    s_dirty_since = aoip_now_ms();
    aoip_unlock();
}

uint32_t settings_skipped_flushes(void) { return s_skipped; }
