// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flow-control client: request / update / stop flows at a transmitter's
// port 4455. Translated from Inferno protocol/flows_control.rs
// (FlowsControlClient). Blocking (3 s reply timeout); subscriber task only.
#include <string.h>
#include "esp_log.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"
#include "aoip/wire.h"

static const char *TAG = "aoip_fcc";

static uint16_t s_seq = 1;

// Send one request and wait for the reply with the same opcode and seqnum.
// Returns the reply's opcode2 (1 = OK) or 0xFFFF on timeout / socket error.
static uint16_t transact(uint32_t ip, uint16_t port, uint16_t op, const uint8_t *content, size_t clen,
                         uint8_t *reply, size_t *reply_len)
{
    static uint8_t pkt[1500];
    if (RR_HEADER_LEN + clen > sizeof pkt) return RR_CODE_TODO_ERROR;
    CTR_INC(fc_requests);
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        CTR_INC(fc_failures);
        return RR_CODE_TODO_ERROR;
    }
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = ip,
    };
    uint16_t code = RR_CODE_TODO_ERROR;
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) goto out;

    const uint16_t seq = s_seq++;
    // start code: Inferno forces 0x1102 ("looks like version number")
    rr_header(pkt, FC_START_CODE, (uint16_t)(RR_HEADER_LEN + clen), seq, op, 0);
    memcpy(pkt + RR_HEADER_LEN, content, clen);
    if (send(s, pkt, RR_HEADER_LEN + clen, 0) < 0) goto out;

    const uint32_t deadline = aoip_now_ms() + FC_REPLY_TIMEOUT_MS;
    for (;;) {
        int32_t left = (int32_t)(deadline - aoip_now_ms());
        if (left <= 0) break;
        struct timeval tv = { .tv_sec = left / 1000, .tv_usec = (left % 1000) * 1000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        int n = recv(s, pkt, sizeof pkt, 0);
        if (n < 0) break;
        if (n < (int)RR_HEADER_LEN) break;
        if (rd16(pkt + 6) == op && rd16(pkt + 4) == seq) {
            code = rd16(pkt + 8);
            if (reply && reply_len) {
                size_t m = (size_t)n - RR_HEADER_LEN;
                if (m > *reply_len) m = *reply_len;
                memcpy(reply, pkt + RR_HEADER_LEN, m);
                *reply_len = m;
            }
            break;
        }
        // spurious packet: keep waiting
    }
out:
    close(s);
    if (code != RR_CODE_OK) CTR_INC(fc_failures);
    return code;
}

uint16_t fcc_request_flow(uint32_t ip, uint16_t port, uint32_t rate, uint32_t bits, uint16_t fpp,
                          const uint16_t *tx_ch, unsigned n, uint16_t rx_port,
                          const char *rx_flow_name, uint8_t handle_out[6])
{
    static uint8_t body_buf[512], str_buf[160];
    bb_t body, str;
    bb_init(&body, body_buf, sizeof body_buf);
    bb_init(&str, str_buf, sizeof str_buf);
    const uint16_t strings_offset = (uint16_t)(FC_REQ_STRINGS_BASE + n * 2u + RR_HEADER_LEN);

    bb_u16(&body, strings_offset);
    bb_str0(&str, g_aoip.friendly_hostname);
    const uint16_t rx_flow_name_offset = (uint16_t)(str.wpos + strings_offset);
    bb_str0(&str, rx_flow_name);
    bb_u32(&body, rate);
    bb_u32(&body, bits);
    bb_u16(&body, 1);
    bb_u16(&body, (uint16_t)n);
    while (!str.ovf && (str.wpos + strings_offset) % 8u != 0) bb_u8(&str, 0);
    bb_u16(&body, (uint16_t)(str.wpos + strings_offset));
    bb_u16(&str, FC_REMOTE_DESCR_0802);
    bb_u16(&str, rx_port);
    uint32_t self_ip = atomic_load(&g_aoip.ip);
    bb_put(&str, &self_ip, 4);
    for (unsigned i = 0; i < n; i++) bb_u16(&body, tx_ch[i]);
    bb_u16(&body, (uint16_t)(FC_REQ_1C + 2u * n));
    bb_u16(&body, FC_REQ_NAMES_0A00);
    bb_u16(&body, FC_REQ_0002);
    bb_u16(&body, fpp);
    bb_u16(&body, rx_flow_name_offset);
    bb_zeros(&body, 12);                // "XXX testing"
    if (body.wpos != strings_offset - RR_HEADER_LEN || body.ovf || str.ovf) {
        ESP_LOGE(TAG, "request_flow layout error");
        return RR_CODE_TODO_ERROR;
    }
    bb_put(&body, str_buf, str.len);
    if (body.ovf) return RR_CODE_TODO_ERROR;

    uint8_t reply[32];
    size_t rlen = sizeof reply;
    uint16_t code = transact(ip, port, FC_OP_REQUEST_FLOW, body_buf, body.len, reply, &rlen);
    if (code == RR_CODE_OK) {
        if (rlen < FC_HANDLE_LEN) return RR_CODE_TODO_ERROR;
        memcpy(handle_out, reply, FC_HANDLE_LEN);
    }
    return code;
}

uint16_t fcc_update_flow(uint32_t ip, uint16_t port, const uint8_t handle[6],
                         const uint16_t *tx_ch, unsigned n)
{
    uint8_t c[FC_HANDLE_LEN + 2 + 2 * AOIP_RX_MAX_CH_PER_FLOW];
    if (n > AOIP_RX_MAX_CH_PER_FLOW) return RR_CODE_TODO_ERROR;
    memcpy(c, handle, FC_HANDLE_LEN);
    wr16(c + 6, (uint16_t)n);
    for (unsigned i = 0; i < n; i++) wr16(c + 8 + 2 * i, tx_ch[i]);
    return transact(ip, port, FC_OP_UPDATE_FLOW, c, 8 + 2 * n, NULL, NULL);
}

uint16_t fcc_stop_flow(uint32_t ip, uint16_t port, const uint8_t handle[6])
{
    return transact(ip, port, FC_OP_STOP_FLOW, handle, FC_HANDLE_LEN, NULL, NULL);
}
