// SPDX-License-Identifier: GPL-3.0-or-later
//
// Multicast TX flows created by the controller (ARC 0x2201/0x2202).
// Translated from Inferno device_server/tx_multicasts.rs.
//
// A bundle gets a random 239.255.x.y:4321 destination, is added to the TX
// flow table as "expired" (not sending) for a grace period, then activated:
// published as <id>@<host>._netaudio-bund and referenced from each member
// channel's _netaudio-chan TXT ("b.<id>=<channel in bundle + 1>").
// Bundles are persisted (NVS "txmc") with their address and restored at boot.
//
// Deviations: Inferno checks for an address conflict during the grace period
// with mDNS A queries for <d>.<c>.<b>.<a>.in-addr.local and publishes that
// name itself; we only wait the same time (MCAST_GRACE_MS) -- TODO. Inferno
// never saves the activated address (FIXME in its source); we do.
#include <string.h>
#include "esp_log.h"

#include "app_config.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"

static const char *TAG = "aoip_txm";

#define SLOTS AOIP_TX_FLOW_SLOTS

typedef struct {
    bool     used;
    bool     active;
    uint8_t  nch;
    int8_t   ch[UI_TX_MAX_CH_PER_FLOW];
    uint32_t dst_ip;
    uint16_t dst_port;
    uint32_t t_added_ms;
} bundle_t;

static bundle_t s_b[SLOTS];

static void save_state(void)
{
    saved_txmc_t mc[SLOTS];
    memset(mc, 0, sizeof mc);
    unsigned k = 0;
    for (unsigned i = 0; i < SLOTS; i++) {
        if (!s_b[i].used) continue;
        mc[k].flow_id = (uint8_t)(i + 1);
        mc[k].nch = s_b[i].nch;
        for (unsigned j = 0; j < s_b[i].nch; j++) {
            mc[k].ch_ids[j] = s_b[i].ch[j] >= 0 ? (uint8_t)(s_b[i].ch[j] + 1) : 0;
        }
        mc[k].dst_ip = s_b[i].dst_ip;
        k++;
    }
    settings_set_txmc(mc);
}

static void refresh_channels(const bundle_t *b)
{
    for (unsigned j = 0; j < b->nch; j++) {
        if (b->ch[j] >= 0) aoip_mdns_refresh_tx_channel((unsigned)b->ch[j]);
    }
}

static bool add_internal(unsigned index, const int8_t *ch, unsigned n, uint32_t preferred_ip)
{
    if (index >= SLOTS || s_b[index].used || n == 0 || n > UI_TX_MAX_CH_PER_FLOW) return false;
    // Clamp at the choke point: ARC and the NVS load validate their channel
    // ids, but this function is also the entry for any future caller, and an
    // out-of-range id would index aoip_mdns_refresh_tx_channel() / the flow
    // table out of bounds downstream.
    int8_t clamped[UI_TX_MAX_CH_PER_FLOW];
    for (unsigned j = 0; j < n; j++) {
        clamped[j] = (ch[j] >= 0 && ch[j] < (int8_t)UI_CHANNELS) ? ch[j] : -1;
    }
    ftx_info_t fi;
    memset(&fi, 0, sizeof fi);
    if (preferred_ip) {
        fi.dst_ip = preferred_ip;
        fi.dst_port = AOIP_PORT_MEDIA_MCAST;
    } else {
        ftx_random_multicast_destination(&fi.dst_ip, &fi.dst_port);
    }
    fi.nch = (uint8_t)n;
    memset(fi.ch, -1, sizeof fi.ch);
    memcpy(fi.ch, clamped, n);
    fi.fpp = TX_FPP_MAX_ADVERTISED;     // "TODO" in Inferno
    fi.bps = UI_NET_BITS / 8u;
    unsigned got;
    uint8_t handle[6];
    if (ftx_add_flow(&fi, (int)index, true, &got, handle) != ESP_OK) {
        ESP_LOGW(TAG, "could not add multicast flow %u", index + 1);
        return false;
    }
    bundle_t *b = &s_b[index];
    memset(b, 0, sizeof *b);
    b->used = true;
    b->nch = (uint8_t)n;
    memset(b->ch, -1, sizeof b->ch);
    memcpy(b->ch, clamped, n);
    b->dst_ip = fi.dst_ip;
    b->dst_port = fi.dst_port;
    b->t_added_ms = aoip_now_ms();
    ESP_LOGI(TAG, "added multicast flow %u (%u ch), waiting grace period", index + 1, n);
    save_state();
    return true;
}

bool txm_add_flow(unsigned flow_index, const int8_t *ch, unsigned n)
{
    return add_internal(flow_index, ch, n, 0);
}

esp_err_t txm_remove_flow(unsigned flow_index)
{
    if (flow_index >= SLOTS || !s_b[flow_index].used) {
        ESP_LOGW(TAG, "removing nonexisting multicast flow %u", flow_index + 1);
        return ESP_ERR_NOT_FOUND;
    }
    // Remove the TX flow FIRST: only when that succeeded may the bundle
    // state (and the persisted copy) forget it. The old order cleared the
    // state up front, so a failed removal left the device streaming a
    // multicast that the persisted + mDNS state no longer knew about.
    const esp_err_t err = ftx_remove_multicast_flow(flow_index);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "removing bundle %u keeps its record (flow removal failed)", flow_index + 1);
        return err;
    }
    bundle_t b = s_b[flow_index];
    s_b[flow_index].used = false;
    s_b[flow_index].active = false;
    if (b.active) aoip_mdns_remove_bundle(flow_index + 1);
    refresh_channels(&b);
    save_state();
    return err;
}

void txm_tick(void)
{
    const uint32_t now = aoip_now_ms();
    for (unsigned i = 0; i < SLOTS; i++) {
        bundle_t *b = &s_b[i];
        if (!b->used || b->active || now - b->t_added_ms < MCAST_GRACE_MS) continue;
        if (!ftx_get(i)) {          // flow vanished meanwhile
            b->used = false;
            save_state();
            continue;
        }
        ESP_LOGI(TAG, "activating multicast flow %u", i + 1);
        b->active = true;
        aoip_mdns_add_bundle(i + 1, b->nch, TX_FPP_MAX_ADVERTISED, b->dst_ip, b->dst_port);
        ftx_activate_multicast_flow(i);
        refresh_channels(b);
        save_state();
    }
}

void txm_load_state(void)
{
    saved_txmc_t mc[SLOTS];
    settings_get_txmc(mc);
    for (unsigned k = 0; k < SLOTS; k++) {
        if (mc[k].flow_id == 0 || mc[k].nch == 0 || mc[k].nch > UI_TX_MAX_CH_PER_FLOW) continue;
        unsigned index = mc[k].flow_id - 1u;
        if (index >= SLOTS || s_b[index].used) {
            unsigned j;
            for (j = 0; j < SLOTS && s_b[j].used; j++) {}
            if (j == SLOTS) {
                ESP_LOGW(TAG, "too many saved multicast flows");
                continue;
            }
            ESP_LOGW(TAG, "loading multicast flows: changing id %u to %u", index + 1, j + 1);
            index = j;
        }
        int8_t ch[UI_TX_MAX_CH_PER_FLOW];
        for (unsigned j = 0; j < mc[k].nch; j++) {
            ch[j] = (mc[k].ch_ids[j] > 0 && mc[k].ch_ids[j] <= UI_CHANNELS) ? (int8_t)(mc[k].ch_ids[j] - 1) : -1;
        }
        uint32_t ip = mc[k].dst_ip;
        // keep only 239.255.0.0/16 addresses we could have chosen
        const uint8_t *a = (const uint8_t *)&ip;
        if (!(a[0] == 239 && a[1] == 255)) ip = 0;
        add_internal(index, ch, mc[k].nch, ip);
    }
}

bool txm_channel_bundle(unsigned tx_ch, unsigned *bundle_id, unsigned *ch_in_bundle)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        const bundle_t *b = &s_b[i];
        if (!b->used || !b->active) continue;
        for (unsigned j = 0; j < b->nch; j++) {
            if (b->ch[j] == (int)tx_ch) {
                *bundle_id = i + 1;
                *ch_in_bundle = j;
                return true;
            }
        }
    }
    return false;
}

void txm_mdns_republish(void)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        const bundle_t *b = &s_b[i];
        if (b->used && b->active) {
            aoip_mdns_add_bundle(i + 1, b->nch, TX_FPP_MAX_ADVERTISED, b->dst_ip, b->dst_port);
        }
    }
}
