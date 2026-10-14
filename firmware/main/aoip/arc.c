// SPDX-License-Identifier: GPL-3.0-or-later
//
// ARC server (port 4440). Translated from Inferno device_server/arc_server.rs
// and protocol/proto_arc.rs; replies are byte-for-byte what Inferno sends
// (including its paging quirk, see wire.c arc_paginate).
//
// Additions over Inferno (marked): device rename (0x1001), empty channel
// name = back to the factory name, bounds checks where Inferno would panic.
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#include "app_config.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"
#include "aoip/wire.h"

static const char *TAG = "aoip_arc";

// ---------------------------------------------------------------------------
// Common channel descriptor (CommonChannelsDescriptor::new)
// ---------------------------------------------------------------------------
static void put_common_descr(bb_t *bb)
{
    bb_u32(bb, UI_SAMPLE_RATE);
    bb_u8(bb, 1);
    bb_u8(bb, 1);
    bb_u16(bb, UI_NET_BITS);
    bb_u16(bb, 0x400);
    bb_u16(bb, UI_NET_BITS);
    bb_u16(bb, UI_NET_BITS);
    bb_u16(bb, AOIP_PCM_TYPE);
}

// ---------------------------------------------------------------------------
// 0x1000 channels and flows count
// ---------------------------------------------------------------------------
static uint16_t op_channels_flows_count(bb_t *bb)
{
    const uint16_t ntx = UI_CHANNELS, nrx = UI_CHANNELS;
    bb_u8(bb, 0);                       // unknown1_0
    // Flags2 (LSB first): bits 0-3 unknown 0, bit 4 supports_tx_channel_rename,
    // bit 5 supports_tx_multicast, bits 6-7 unknown 0
    bb_u8(bb, 0x30);
    bb_u16(bb, ntx);
    bb_u16(bb, nrx);
    bb_u16(bb, 4);                      // unknown2_4 "or 1"
    bb_u16(bb, ntx < UI_TX_MAX_CH_PER_FLOW ? ntx : UI_TX_MAX_CH_PER_FLOW);
    bb_u16(bb, 8);                      // unknown4_8
    bb_u16(bb, AOIP_TX_FLOW_SLOTS);
    bb_u16(bb, AOIP_RX_FLOW_SLOTS);
    bb_u16(bb, (uint16_t)(ntx + nrx));  // "??? not actually total number of channels"
    bb_u16(bb, 1);
    bb_u16(bb, 1);
    bb_zeros(bb, 12);
    return RR_CODE_OK;
}

// ---------------------------------------------------------------------------
// 0x1003 device names
// ---------------------------------------------------------------------------
static uint16_t op_device_names(bb_t *bb)
{
    const uint16_t hdr = bb->wpos;
    bb_zeros(bb, ARC_SZ_DEVICE_NAMES_HDR);
    uint16_t friendly = bb_str0(bb, g_aoip.friendly_hostname);
    uint16_t factory = bb_str0(bb, g_aoip.factory_hostname);
    uint16_t board = bb_str0(bb, AOIP_BOARD_NAME);
    uint16_t rev = bb_str0(bb, ARC_REVISION_STRING);
    if (bb->ovf) return RR_CODE_OK;
    uint8_t *h = bb->b + hdr;
    // unknown1_0, unknown2_0, unknown3_0, board, revision, unknown4_0,
    // friendly1, factory, friendly2, [6] 0, start_code, 0, 0x1102, 0
    wr16(h + 6, board);
    wr16(h + 8, rev);
    wr16(h + 12, friendly);
    wr16(h + 14, factory);
    wr16(h + 16, friendly);
    wr16(h + 30, ARC_DEVICE_NAMES_START_CODE);
    wr16(h + 34, ARC_DEVICE_NAMES_OP_1102);
    return RR_CODE_OK;
}

// ---------------------------------------------------------------------------
// 0x3000 receive channels (names and subscriptions)
// ---------------------------------------------------------------------------
typedef struct { uint16_t common_off; } chlist_ctx_t;

static bool rx_channel_item(bb_t *bb, size_t i, uint8_t *out, void *vctx)
{
    chlist_ctx_t *ctx = vctx;
    if (i >= UI_CHANNELS) return false;
    if (ctx->common_off == 0) {
        ctx->common_off = bb->wpos;
        put_common_descr(bb);
    }
    sub_info_t si;
    sub_get_info((unsigned)i, &si);
    uint16_t txch_off = 0, host_off = 0;
    uint32_t status = 0;
    if (si.present) {
        txch_off = bb_str0(bb, si.txch);
        host_off = bb_str0(bb, si.host);
        status = si.status;
    }
    uint16_t friendly_off = bb_str0(bb, g_aoip.rx_names[i]);
    wr16(out + 0, (uint16_t)(i + 1));
    wr16(out + 2, 6);
    wr16(out + 4, ctx->common_off);
    wr16(out + 6, txch_off);
    wr16(out + 8, host_off);
    wr16(out + 10, friendly_off);
    wr32(out + 12, status);
    wr32(out + 16, 0);
    return true;
}

// ---------------------------------------------------------------------------
// 0x2000 transmit channels (factory names)
// ---------------------------------------------------------------------------
static bool tx_channel_item(bb_t *bb, size_t i, uint8_t *out, void *vctx)
{
    chlist_ctx_t *ctx = vctx;
    if (i >= UI_CHANNELS) return false;
    if (ctx->common_off == 0) {
        ctx->common_off = bb->wpos;
        put_common_descr(bb);
    }
    char fac[4];
    aoip_factory_ch_name(fac, (unsigned)i);
    wr16(out + 0, (uint16_t)(i + 1));
    wr16(out + 2, 7);
    wr16(out + 4, ctx->common_off);
    wr16(out + 6, bb_str0(bb, fac));
    return true;
}

// ---------------------------------------------------------------------------
// 0x2010 transmit channels friendly names
// ---------------------------------------------------------------------------
typedef struct { bool wrote; } friendly_ctx_t;

static bool tx_friendly_item(bb_t *bb, size_t i, uint8_t *out, void *vctx)
{
    friendly_ctx_t *ctx = vctx;
    if (i >= UI_CHANNELS) return false;
    if (!ctx->wrote) {
        bb_u32(bb, 0);
        ctx->wrote = true;
    }
    wr16(out + 0, (uint16_t)(i + 1));
    wr16(out + 2, (uint16_t)(i + 1));
    wr16(out + 4, bb_str0(bb, g_aoip.tx_names[i]));
    return true;
}

// ---------------------------------------------------------------------------
// Renames (0x2013 TX, 0x3001 RX)
// ---------------------------------------------------------------------------
static void set_channel_name(char *dst, unsigned index, const char *name)
{
    // Not in Inferno: an empty name restores the factory name.
    if (name[0]) {
        strncpy(dst, name, AOIP_NAME_LEN - 1);
        dst[AOIP_NAME_LEN - 1] = 0;
    } else {
        aoip_factory_ch_name(dst, index);
    }
}

static uint16_t op_rename_tx(bb_t *bb, const uint8_t *c, size_t clen)
{
    bool renamed = false;
    size_t n = clen >= 2 ? c[1] : 0;
    if (n > clen / ARC_SZ_RENAME_TX_REQ) n = clen / ARC_SZ_RENAME_TX_REQ;
    for (size_t k = 0; k < n; k++) {
        const uint8_t *it = c + 2 + k * ARC_SZ_RENAME_TX_REQ;
        if ((size_t)(it - c) + ARC_SZ_RENAME_TX_REQ > clen) break;
        uint16_t id = rd16(it + 2);
        uint16_t off = rd16(it + 4);
        size_t name_off = off > RR_HEADER_LEN ? off - RR_HEADER_LEN : 0;
        if (id == 0 || name_off == 0) continue;
        char name[AOIP_NAME_LEN];
        if (!rd_str0(c, clen, name_off, name, sizeof name)) continue;
        unsigned index = id - 1u;
        if (index >= UI_CHANNELS) {
            ESP_LOGW(TAG, "rename TX: invalid channel %u", id);
            continue;
        }
        // A name another TX channel already uses (as its factory "01".."NN"
        // or friendly name) would make the mDNS add fail now and a later
        // rename remove the OTHER channel's service: refuse it.
        bool clash = false;
        for (unsigned j = 0; j < UI_CHANNELS && name[0]; j++) {
            char fac[4];
            aoip_factory_ch_name(fac, j);
            if (j != index && (strcmp(name, fac) == 0 || strcmp(name, g_aoip.tx_names[j]) == 0)) clash = true;
        }
        if (clash) {
            ESP_LOGW(TAG, "rename TX: '%s' is in use by another channel", name);
            continue;
        }
        ESP_LOGI(TAG, "renaming TX channel %u to '%s'", id, name);
        char old[AOIP_NAME_LEN];
        strcpy(old, g_aoip.tx_names[index]);
        set_channel_name(g_aoip.tx_names[index], index, name);
        aoip_mdns_remove_tx_channel_name(index, old);
        aoip_mdns_refresh_tx_channel(index);
        settings_mark_dirty(SET_DIRTY_NAMES);
        renamed = true;
    }
    if (renamed) {
        bb_u16(bb, 0);  // "sometimes it is [0, 1, 0, 0, H(id), L(id)]"
        return RR_CODE_OK;
    }
    return RR_CODE_TODO_ERROR;
}

static uint16_t op_rename_rx(const uint8_t *c, size_t clen)
{
    uint32_t renamed = 0;
    size_t n = clen >= 2 ? c[1] : 0;
    if (n > clen / ARC_SZ_RENAME_RX_REQ) n = clen / ARC_SZ_RENAME_RX_REQ;
    for (size_t k = 0; k < n; k++) {
        const uint8_t *it = c + 2 + k * ARC_SZ_RENAME_RX_REQ;
        if ((size_t)(it - c) + ARC_SZ_RENAME_RX_REQ > clen) break;
        uint16_t id = rd16(it + 0);
        uint16_t off = rd16(it + 2);
        size_t name_off = off > RR_HEADER_LEN ? off - RR_HEADER_LEN : 0;
        if (id == 0 || name_off == 0) continue;
        char name[AOIP_NAME_LEN];
        if (!rd_str0(c, clen, name_off, name, sizeof name)) continue;
        unsigned index = id - 1u;
        if (index >= UI_CHANNELS) {
            ESP_LOGW(TAG, "rename RX: invalid channel %u", id);
            continue;
        }
        ESP_LOGI(TAG, "renaming RX channel %u to '%s'", id, name);
        set_channel_name(g_aoip.rx_names[index], index, name);
        renamed |= 1u << index;
    }
    if (renamed) settings_mark_dirty(SET_DIRTY_NAMES);
    // Inferno sends the channel-change notification in every case
    info_notify_rx_channels(renamed);
    return renamed ? RR_CODE_OK : RR_CODE_TODO_ERROR;
}

// ---------------------------------------------------------------------------
// 0x2200 query TX flows
// ---------------------------------------------------------------------------
static bool tx_flow_item(bb_t *bb, size_t i, uint8_t *out, void *vctx)
{
    const ftx_info_t *f = ftx_get((unsigned)i);
    if (!f) return false;
    char flow_name[16];
    snprintf(flow_name, sizeof flow_name, "%u_%u", (unsigned)(i + 1), g_aoip.process_id);
    uint16_t local_name_off = bb_str0(bb, flow_name);
    uint16_t rhost_off = bb_str0_or_0(bb, f->multicast ? NULL : f->rx_hostname);
    uint16_t rflow_off = bb_str0_or_0(bb, f->multicast ? NULL : f->rx_flow_name);
    bool is_multicast = rhost_off == 0 && rflow_off == 0;

    bb_align(bb, 4);
    uint16_t rsd_off = bb->wpos;
    bb_u16(bb, ARC_UNKNOWN_DEST_8002);
    bb_u16(bb, f->dst_port);
    bb_put(bb, &f->dst_ip, 4);

    uint16_t names_off = bb->wpos;
    bb_u16(bb, 0xa00);
    bb_u16(bb, 1);
    bb_u16(bb, rhost_off);
    bb_u16(bb, rflow_off);
    bb_u16(bb, 0x10);                   // "or 0x3c ???"
    bb_u16(bb, local_name_off);
    bb_zeros(bb, 8);                    // "in multicast flows first 4B are latency_ns"

    uint16_t main_off = bb->wpos;
    bb_u16(bb, (uint16_t)(i + 1));
    bb_u16(bb, is_multicast ? ARC_TXF_TYPE_MULTICAST : ARC_TXF_TYPE_UNICAST);
    bb_u32(bb, UI_SAMPLE_RATE);
    bb_u16(bb, 0);
    bb_u16(bb, UI_NET_BITS);            // self_info.bits_per_sample, as Inferno
    bb_u16(bb, 1);
    bb_u16(bb, f->nch);
    bb_u16(bb, rsd_off);
    for (unsigned k = 0; k < f->nch; k++) bb_u16(bb, f->ch[k] >= 0 ? (uint16_t)(f->ch[k] + 1) : 0);
    bb_u16(bb, names_off);
    wr16(out, main_off);
    return true;
}

// ---------------------------------------------------------------------------
// 0x2201 create / 0x2202 delete multicast TX flow
// ---------------------------------------------------------------------------
static uint16_t op_create_mcast(bb_t *bb, const uint8_t *c, size_t clen)
{
    uint16_t ids[AOIP_TX_FLOW_SLOTS];
    unsigned nids = 0;
    size_t n = clen >= 2 ? c[1] : 0;
    if (n > clen / 2) n = clen / 2;
    for (size_t k = 0; k < n; k++) {
        if (2 + k * 2 + 2 > clen) break;
        size_t off = rd16(c + 2 + k * 2);
        if (off < RR_HEADER_LEN) continue;
        size_t d = off - RR_HEADER_LEN;
        if (d + ARC_SZ_MCAST_CREATE_HEADER > clen) continue;
        uint16_t flow_id = rd16(c + d);
        uint16_t flow_type = rd16(c + d + 2);
        uint16_t nch = rd16(c + d + 14);
        if (flow_type != ARC_TXF_TYPE_MULTICAST) {
            ESP_LOGW(TAG, "create unknown flow type %u", flow_type);
            continue;
        }
        if (flow_id == 0 || flow_id > AOIP_TX_FLOW_SLOTS) {
            ESP_LOGW(TAG, "create multicast flow: invalid id %u", flow_id);
            continue;
        }
        unsigned index = flow_id - 1u;
        if (ftx_get(index)) {
            ESP_LOGW(TAG, "tx flow id busy: %u", flow_id);
            continue;
        }
        const uint8_t *after = c + d + ARC_SZ_MCAST_CREATE_HEADER;
        size_t after_len = clen - d - ARC_SZ_MCAST_CREATE_HEADER;
        if (after_len < (size_t)nch * 2u + ARC_SZ_MCAST_CREATE_FOOTER || nch == 0 ||
            nch > UI_TX_MAX_CH_PER_FLOW) {
            ESP_LOGW(TAG, "multicast flow descriptor: bad channel list (%u)", nch);
            continue;
        }
        int8_t ch[UI_TX_MAX_CH_PER_FLOW];
        for (unsigned j = 0; j < nch; j++) {
            uint16_t id = rd16(after + j * 2);
            ch[j] = (id > 0 && id <= UI_CHANNELS) ? (int8_t)(id - 1) : -1;
        }
        if (!txm_add_flow(index, ch, nch)) continue;
        if (nids < AOIP_TX_FLOW_SLOTS) ids[nids++] = (uint16_t)(index + 1);
    }
    if (nids == 0) return RR_CODE_TODO_ERROR;
    bb_u16(bb, (uint16_t)nids);
    bb_u16(bb, 0);
    for (unsigned k = 0; k < nids; k++) bb_u16(bb, ids[k]);
    return RR_CODE_OK;
}

static uint16_t op_delete_mcast(const uint8_t *c, size_t clen)
{
    if (clen < 4) return RR_CODE_TODO_ERROR;
    size_t count = rd16(c);
    bool deleted = false;
    for (size_t k = 0; k < count && 4 + k * 2 + 2 <= clen; k++) {
        uint16_t id = rd16(c + 4 + k * 2);
        if (id == 0) continue;
        if (txm_remove_flow(id - 1u) == ESP_OK) {
            deleted = true;
            ESP_LOGI(TAG, "deleted multicast tx flow id %u", id);
        }
    }
    return deleted ? RR_CODE_OK : RR_CODE_TODO_ERROR;
}

// ---------------------------------------------------------------------------
// 0x3200 query RX flows
// ---------------------------------------------------------------------------
static bool rx_flow_item(bb_t *bb, size_t i, uint8_t *out, void *vctx)
{
    frx_info_t fi;
    if (!frx_get_info((unsigned)i, &fi)) return false;

    bb_align(bb, 4);
    uint16_t rsd_off = bb->wpos;
    uint32_t ip = atomic_load(&g_aoip.ip);
    bb_u16(bb, ARC_UNKNOWN_DEST_8002);
    bb_u16(bb, fi.rx_port);
    bb_put(bb, &ip, 4);

    uint16_t d2_off = bb->wpos;
    bb_u16(bb, 9);
    bb_u16(bb, 1);
    bb_u16(bb, 0x800);
    bb_u16(bb, 0);
    bb_u32(bb, (uint32_t)((uint64_t)fi.latency_frames * 1000000000ull / UI_SAMPLE_RATE));
    bb_u32(bb, 0);

    // Each channel's BoolVec has rx_channels.len() bits -> 2 words for 32 channels
    const unsigned words = (UI_CHANNELS + 15u) / 16u;
    uint16_t mask_off[AOIP_RX_MAX_CH_PER_FLOW];
    for (unsigned k = 0; k < fi.nch; k++) {
        mask_off[k] = bb->wpos;
        for (unsigned w = 0; w < words; w++) {
            bb_u16(bb, (uint16_t)(fi.channels_map[k] >> (16u * w)));
        }
    }

    bb_align(bb, 4);
    uint16_t main_off = bb->wpos;
    bb_u16(bb, (uint16_t)(i + 1));
    bb_u16(bb, 1);
    bb_u32(bb, UI_SAMPLE_RATE);
    bb_u16(bb, 0);
    bb_u16(bb, UI_NET_BITS);
    bb_u16(bb, 1);
    bb_u16(bb, fi.nch);
    bb_u16(bb, (uint16_t)words);
    bb_u16(bb, rsd_off);
    for (unsigned k = 0; k < fi.nch; k++) bb_u16(bb, mask_off[k]);
    bb_u16(bb, d2_off);
    wr16(out, main_off);
    return true;
}

// ---------------------------------------------------------------------------
// 0x3010 subscriptions, 0x3014 netaudio unsubscribe
// ---------------------------------------------------------------------------
static void op_set_subscriptions(const uint8_t *c, size_t clen)
{
    size_t n = clen >= 2 ? c[1] : 0;
    if (n > clen / ARC_SZ_SUBSCRIPTION_REQ) n = clen / ARC_SZ_SUBSCRIPTION_REQ;
    for (size_t k = 0; k < n; k++) {
        const uint8_t *it = c + 2 + k * ARC_SZ_SUBSCRIPTION_REQ;
        if ((size_t)(it - c) + ARC_SZ_SUBSCRIPTION_REQ > clen) break;
        uint16_t local_id = rd16(it);
        uint16_t txch_off = rd16(it + 2);
        uint16_t host_off = rd16(it + 4);
        if (local_id == 0) continue;
        unsigned lci = local_id - 1u;
        if (lci >= UI_CHANNELS) {
            ESP_LOGW(TAG, "connect/disconnect for nonexisting channel %u", local_id);
            continue;
        }
        if (txch_off > 0 && host_off > 0) {
            char txch[AOIP_NAME_LEN], host[AOIP_NAME_LEN];
            bool ok1 = txch_off >= RR_HEADER_LEN &&
                       rd_str0(c, clen, txch_off - RR_HEADER_LEN, txch, sizeof txch);
            bool ok2 = host_off >= RR_HEADER_LEN &&
                       rd_str0(c, clen, host_off - RR_HEADER_LEN, host, sizeof host);
            if (ok1 && ok2) {
                ESP_LOGI(TAG, "connection requested: %u <- %s@%s", local_id, txch, host);
                sub_subscribe(lci, txch, host);
            } else {
                ESP_LOGW(TAG, "couldn't read tx names from subscription request");
            }
        } else {
            ESP_LOGI(TAG, "disconnect requested: local channel %u", local_id);
            sub_unsubscribe(lci);
        }
    }
}

// ---------------------------------------------------------------------------
// 0x1001 set device name (NOT in Inferno; network-audio-controller command)
// ---------------------------------------------------------------------------
static bool valid_name_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '-';
}

static uint16_t op_set_device_name(const uint8_t *c, size_t clen)
{
    char name[AOIP_NAME_LEN] = { 0 };
    if (clen > 0) rd_str0(c, clen, 0, name, sizeof name);
    if (!name[0]) {
        strcpy(name, g_aoip.default_hostname);
    }
    for (const char *p = name; *p; p++) {
        if (!valid_name_char(*p)) {
            ESP_LOGW(TAG, "rename device: invalid name '%s'", name);
            return RR_CODE_TODO_ERROR;
        }
    }
    if (strcmp(name, g_aoip.friendly_hostname) != 0) {
        ESP_LOGI(TAG, "renaming device '%s' -> '%s'", g_aoip.friendly_hostname, name);
        strcpy(g_aoip.friendly_hostname, name);
        settings_mark_dirty(SET_DIRTY_NAMES);
        // Full republish (~70 mDNS ops) is deferred past the serve() lock;
        // the ctl loop picks it up after unlock.
        aoip_mdns_request_republish();
    }
    return RR_CODE_OK;
}

// ---------------------------------------------------------------------------
// 0x1100 device settings, 0x1102 property directory
// ---------------------------------------------------------------------------
// Latency bounds announced to controllers (fork MIN/MAX_LATENCY_NS). The
// device latency itself is fixed by the build profile (UI_NET_LATENCY_NS),
// so default, configured and active all report it.
#define ARC_LAT_MIN_NS              500000u
#define ARC_LAT_MAX_NS              10000000u

static uint16_t op_device_settings(bb_t *bb)
{
    // fork device_settings_response: [02][count], then (id, value offset
    // from the start of the packet) records, then the u32BE values: sample
    // rate (0x8020) and default/configured/active/maximum/minimum latency
    // in ns (0x8204/0x8205/0x8301/0x8302/0x8306).
    static const uint16_t ids[6] = { 0x8020u, 0x8204u, 0x8205u, 0x8301u, 0x8302u, 0x8306u };
    const uint32_t vals[6] = {
        UI_SAMPLE_RATE, UI_NET_LATENCY_NS, UI_NET_LATENCY_NS,
        UI_NET_LATENCY_NS, ARC_LAT_MAX_NS, ARC_LAT_MIN_NS,
    };
    bb_u8(bb, 2);
    bb_u8(bb, 6);
    for (unsigned i = 0; i < 6; i++) {
        bb_u16(bb, ids[i]);
        bb_u16(bb, (uint16_t)(RR_HEADER_LEN + 2u + 6u * 4u + i * 4u));
    }
    for (unsigned i = 0; i < 6; i++) bb_u32(bb, vals[i]);
    return RR_CODE_OK;
}

static uint16_t op_property_directory(bb_t *bb)
{
    // fork property_directory_response: u16 count, then (id, flags) pairs;
    // flags 1 = read, 3 = read/write. The set a hardware interface announces.
    static const uint16_t props[31][2] = {
        { 0x8020u, 1 }, { 0x8021u, 3 }, { 0x0022u, 3 }, { 0x0023u, 3 },
        { 0x0024u, 1 }, { 0x8060u, 3 }, { 0x0062u, 3 }, { 0x0063u, 1 },
        { 0x0201u, 3 }, { 0x8204u, 3 }, { 0x8205u, 3 }, { 0x020au, 1 },
        { 0x020bu, 1 }, { 0x0210u, 3 }, { 0x0211u, 3 }, { 0x0212u, 3 },
        { 0x0213u, 1 }, { 0x0214u, 1 }, { 0x0222u, 3 }, { 0x8301u, 3 },
        { 0x8306u, 1 }, { 0x8302u, 1 }, { 0x8321u, 1 }, { 0x0310u, 1 },
        { 0x0311u, 1 }, { 0x0312u, 1 }, { 0x0303u, 3 }, { 0x83f0u, 1 },
        { 0x0601u, 1 }, { 0x0309u, 1 }, { 0x0209u, 1 },
    };
    bb_u16(bb, 31);
    for (unsigned i = 0; i < 31; i++) {
        bb_u16(bb, props[i][0]);
        bb_u16(bb, props[i][1]);
    }
    return RR_CODE_OK;
}

// ---------------------------------------------------------------------------
size_t arc_handle(const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    if (len < RR_HEADER_LEN) return 0;
    if (rd16(req + 8) != 0) {
        ESP_LOGD(TAG, "unknown opcode2 0x%04x", rd16(req + 8));
        return 0;
    }
    const uint16_t op = rd16(req + 6);
    const uint8_t *c = req + RR_HEADER_LEN;
    const size_t clen = len - RR_HEADER_LEN;

    bb_t bb;
    bb_init(&bb, out, cap);
    bb_set_wpos(&bb, RR_HEADER_LEN);
    uint16_t code = RR_CODE_OK;

    switch (op) {
    case ARC_OP_CHANNELS_FLOWS_COUNT:
        code = op_channels_flows_count(&bb);
        break;
    case ARC_OP_GET_DEVICE_NAME:
        bb_str0(&bb, g_aoip.friendly_hostname);
        break;
    case ARC_OP_GET_DEVICE_NAMES:
        code = op_device_names(&bb);
        break;
    case ARC_OP_SET_DEVICE_NAME:
        code = op_set_device_name(c, clen);
        break;
    case ARC_OP_GET_RX_CHANNELS: {
        chlist_ctx_t ctx = { 0 };
        code = arc_paginate(&bb, c, clen, UI_CHANNELS, ARC_SZ_RX_CH_DESCR, UI_CHANNELS,
                            rx_channel_item, &ctx);
        break;
    }
    case ARC_OP_GET_TX_CHANNELS: {
        chlist_ctx_t ctx = { 0 };
        code = arc_paginate(&bb, c, clen, UI_CHANNELS, ARC_SZ_TX_CH_DESCR, UI_CHANNELS,
                            tx_channel_item, &ctx);
        break;
    }
    case ARC_OP_GET_TX_FRIENDLY: {
        friendly_ctx_t ctx = { 0 };
        code = arc_paginate(&bb, c, clen, UI_CHANNELS, ARC_SZ_TX_FRIENDLY_DESCR, UI_CHANNELS,
                            tx_friendly_item, &ctx);
        break;
    }
    case ARC_OP_RENAME_TX_CHANNELS:
        code = op_rename_tx(&bb, c, clen);
        break;
    case ARC_OP_RENAME_RX_CHANNELS:
        code = op_rename_rx(c, clen);
        break;
    case ARC_OP_QUERY_TX_FLOWS:
        code = arc_paginate(&bb, c, clen, AOIP_TX_FLOW_SLOTS < 16 ? AOIP_TX_FLOW_SLOTS : 16, 2,
                            AOIP_TX_FLOW_SLOTS, tx_flow_item, NULL);
        break;
    case ARC_OP_CREATE_MCAST_TX_FLOW:
        code = op_create_mcast(&bb, c, clen);
        break;
    case ARC_OP_DELETE_MCAST_TX_FLOW:
        code = op_delete_mcast(c, clen);
        break;
    case ARC_OP_UNKNOWN_2320:
        code = ARC_CODE_2320;
        break;
    case ARC_OP_QUERY_RX_FLOWS:
        code = arc_paginate(&bb, c, clen, AOIP_RX_FLOW_SLOTS < 16 ? AOIP_RX_FLOW_SLOTS : 16, 2,
                            AOIP_RX_FLOW_SLOTS, rx_flow_item, NULL);
        break;
    case ARC_OP_DEVICE_SETTINGS:
        code = op_device_settings(&bb);
        break;
    case ARC_OP_DEVICE_SETTINGS_WRITE:
        // Latency is a build-time profile on this device (no 0x1101 write
        // path exists); refuse cleanly instead of timing the controller out.
        code = ARC_CODE_SETTINGS_REFUSED;
        break;
    case ARC_OP_PROPERTY_DIRECTORY:
        code = op_property_directory(&bb);
        break;
    case ARC_OP_TX_FLOW_LABELS:
        // The device keeps no flow labels: an empty page, as netaudio's own
        // virtual device answers it (fork 1491622). Ends the controller's
        // per-poll wait for this query.
        bb_u8(&bb, 2);   // page size marker
        bb_u8(&bb, 0);   // no records
        break;
    case ARC_OP_UNKNOWN_3300: {
        const uint8_t r[8] = ARC_3300_REPLY;
        bb_put(&bb, r, sizeof r);
        break;
    }
    case ARC_OP_SET_SUBSCRIPTIONS:
        op_set_subscriptions(c, clen);
        break;
    case ARC_OP_NETAUDIO_UNSUBSCRIBE: {
        if (clen < 6) return 0;
        uint16_t local = rd16(c + 4);
        if (local == 0 || local > UI_CHANNELS) return 0;
        ESP_LOGI(TAG, "disconnect requested: local channel %u", local);
        sub_unsubscribe(local - 1u);
        break;
    }
    default:
        ESP_LOGD(TAG, "unknown opcode1 0x%04x (%u bytes)", op, (unsigned)clen);
        return 0;
    }

    if (bb.ovf) {
        ESP_LOGE(TAG, "reply to 0x%04x overflowed", op);
        return 0;
    }
    if (code == RR_CODE_TODO_ERROR) bb.len = RR_HEADER_LEN;   // error replies are empty
    rr_finish(out, req, code, bb.len);
    return bb.len;
}
