// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flow-control server (port 4455): receivers request, update and stop our
// TX flows. Translated from Inferno device_server/flows_control_server.rs.
//
// Request flow (0x0100) content, offsets relative to the content unless
// noted (strings and the remote descriptor are offsets from the start of the
// packet, header included):
//   u16 hostname_offset, u32 sample_rate, u32 bits_per_sample, u16 1,
//   u16 num_channels, u16 remote_descr_offset, u16 channel_id[n],
//   6 bytes (u16 0x1c+2n, u16 0x0a00, u16 2), u16 fpp, u16 rx_flow_name_offset
//   remote descriptor: u16 0x0802, u16 rx_port, [4] rx_ip
// Reply: 6-byte flow handle (code 1) or an error code with no content.
//
// Checks beyond Inferno (it would panic or accept): string/descriptor
// offsets in range, channel count <= UI_TX_MAX_CH_PER_FLOW, encoding 16/24/32,
// packet fits the MTU. Inferno answers a bad channel number with 0x0302 but
// then still adds the flow; we stop there.
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"

#include "app_config.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"
#include "aoip/wire.h"

static const char *TAG = "aoip_fcs";

#define MAX_RAW_UDP_PAYLOAD 1472u   // net_udp_send_raw limit

static size_t reply(uint8_t *out, const uint8_t *req, uint16_t code, const uint8_t *content, size_t n)
{
    if (n) memcpy(out + RR_HEADER_LEN, content, n);
    rr_finish(out, req, code, (uint16_t)(RR_HEADER_LEN + n));
    return RR_HEADER_LEN + n;
}

static size_t request_flow(const uint8_t *req, size_t len, uint8_t *out)
{
    const uint8_t *c = req + RR_HEADER_LEN;
    const size_t clen = len - RR_HEADER_LEN;
    if (clen <= 16) {
        ESP_LOGW(TAG, "invalid flow request, too short");
        return 0;
    }
    const size_t host_off = rd16(c + 0);
    const uint32_t rate = rd32(c + 2);
    const uint32_t bits = rd32(c + 6);
    const uint16_t nch = rd16(c + 12);
    const size_t descr_off = rd16(c + 14);
    const size_t fpp_at = 16u + (size_t)nch * 2u + 6u;
    if (fpp_at + 4u > clen) {
        ESP_LOGW(TAG, "flow request truncated (%u channels)", nch);
        return 0;
    }
    const uint16_t fpp = rd16(c + fpp_at);
    const size_t flow_name_off = rd16(c + fpp_at + 2);

    ftx_info_t fi;
    memset(&fi, 0, sizeof fi);
    if (!rd_str0(req, len, host_off, fi.rx_hostname, sizeof fi.rx_hostname) ||
        !rd_str0(req, len, flow_name_off, fi.rx_flow_name, sizeof fi.rx_flow_name)) {
        ESP_LOGW(TAG, "flow request: bad string offsets");
        return 0;
    }
    if (len < descr_off + 8u) {
        ESP_LOGW(TAG, "flow request: packet too short for remote descriptor");
        return 0;
    }
    if (rd16(req + descr_off) != FC_REMOTE_DESCR_0802) {
        ESP_LOGW(TAG, "expected 0x0802, got 0x%04x", rd16(req + descr_off));
    }
    fi.dst_port = rd16(req + descr_off + 2);
    memcpy(&fi.dst_ip, req + descr_off + 4, 4);

    ESP_LOGI(TAG, "%s requests flow %s: %u ch, %lu Hz, %lu bit, fpp %u -> " IPSTR ":%u",
             fi.rx_hostname, fi.rx_flow_name, nch, (unsigned long)rate, (unsigned long)bits, fpp,
             IP2STR((esp_ip4_addr_t *)&fi.dst_ip), fi.dst_port);

    if (nch == 0 || nch > UI_TX_MAX_CH_PER_FLOW) {
        return reply(out, req, FC_ERR_0302, NULL, 0);
    }
    memset(fi.ch, -1, sizeof fi.ch);
    for (unsigned i = 0; i < nch; i++) {
        uint16_t id = rd16(c + 16 + i * 2);
        if (id > UI_CHANNELS) {
            ESP_LOGW(TAG, "too large channel number %u", id);
            return reply(out, req, FC_ERR_0302, NULL, 0);
        }
        fi.ch[i] = id ? (int8_t)(id - 1) : -1;
    }
    fi.nch = (uint8_t)nch;
    if (rate != UI_SAMPLE_RATE) {
        ESP_LOGW(TAG, "sample rate mismatch (%lu)", (unsigned long)rate);
        return reply(out, req, FC_ERR_SAMPLE_RATE_MISMATCH, NULL, 0);
    }
    if (fpp > TX_FPP_MAX || fpp < TX_FPP_MIN) {     // fpp 1 = 48k packets/s per flow
        return reply(out, req, FC_ERR_0302, NULL, 0);
    }
    if (bits != 16 && bits != 24 && bits != 32) {
        return reply(out, req, FC_ERR_0302, NULL, 0);
    }
    fi.bps = (uint8_t)(bits / 8u);
    fi.fpp = fpp;
    if (MEDIA_HEADER_LEN + (size_t)fpp * nch * fi.bps > MAX_RAW_UDP_PAYLOAD) {
        ESP_LOGW(TAG, "flow does not fit the MTU");
        return reply(out, req, FC_ERR_0302, NULL, 0);
    }

    unsigned index;
    uint8_t handle[FC_HANDLE_LEN];
    esp_err_t err = ftx_add_flow(&fi, -1, false, &index, handle);
    if (err != ESP_OK) {
        return reply(out, req, FC_ERR_TOO_MANY_TX_FLOWS, NULL, 0);
    }
    return reply(out, req, RR_CODE_OK, handle, sizeof handle);
}

size_t fcs_handle(const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    if (len < RR_HEADER_LEN || cap < 64) return 0;
    if (rd16(req + 8) != 0) return 0;
    const uint8_t *c = req + RR_HEADER_LEN;
    const size_t clen = len - RR_HEADER_LEN;

    switch (rd16(req + 6)) {
    case FC_OP_REQUEST_FLOW:
        return request_flow(req, len, out);
    case FC_OP_STOP_FLOW:
        if (clen != FC_HANDLE_LEN) {
            ESP_LOGW(TAG, "stop flow: bad length %u", (unsigned)clen);
            return 0;
        }
        if (ftx_remove_flow(c) == ESP_OK) {
            ESP_LOGI(TAG, "stopped flow %lu", (unsigned long)rd32(c));
            return reply(out, req, RR_CODE_OK, NULL, 0);
        }
        return reply(out, req, FC_ERR_FLOW_NOT_FOUND, NULL, 0);
    case FC_OP_UPDATE_FLOW: {
        if (clen < 8) return 0;
        unsigned n = rd16(c + 6);
        if (clen < 8u + n * 2u) return 0;
        if (n > UI_TX_MAX_CH_PER_FLOW) return reply(out, req, FC_ERR_0302, NULL, 0);
        int8_t ch[UI_TX_MAX_CH_PER_FLOW];
        for (unsigned i = 0; i < n; i++) {
            uint16_t id = rd16(c + 8 + i * 2);
            ch[i] = (id > 0 && id <= UI_CHANNELS) ? (int8_t)(id - 1) : -1;
        }
        esp_err_t err = ftx_set_channels(c, ch, n);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "set %u channels in flow %lu", n, (unsigned long)rd32(c));
            return reply(out, req, RR_CODE_OK, NULL, 0);
        }
        if (err == ESP_ERR_INVALID_SIZE) return reply(out, req, FC_ERR_0302, NULL, 0);
        return reply(out, req, FC_ERR_FLOW_NOT_FOUND, NULL, 0);
    }
    default:
        return 0;
    }
}
