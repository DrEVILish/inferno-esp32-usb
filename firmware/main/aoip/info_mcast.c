// SPDX-License-Identifier: GPL-3.0-or-later
//
// Info multicasts and heartbeat. Translated from Inferno
// device_server/info_mcast_server.rs and protocol/mcast.rs.
//
//   to 224.0.0.231:8702 (from port 8700): board info, product info, network
//       info, clock stats, channel-change notifications, reply 0x78
//   to 224.0.0.233:8708: heartbeat, once a second
//   unicast requests arrive on port 8700 (opcode in bytes 24..31)
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "audio/ring.h"
#include "clock/mediaclock.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"
#include "aoip/wire.h"

static const char *TAG = "aoip_info";

static int s_sock = -1;
static uint16_t s_seq = 1;
static QueueHandle_t s_notify_q;
static uint8_t s_pkt[1500];

void info_set_socket(int sock)
{
    s_sock = sock;
    if (!s_notify_q) s_notify_q = xQueueCreate(32, sizeof(uint32_t));
}

static void put_str(uint8_t *buf, size_t off, size_t max, const char *s)
{
    // write_str_to_buffer: copy min(max, len) bytes, no terminator
    size_t n = strlen(s);
    if (n > max) n = max;
    memcpy(buf + off, s, n);
}

static void mc_send(uint32_t dst_host, uint16_t port, uint16_t start_code, const uint8_t opcode[8],
                    const uint8_t *content, size_t n)
{
    if (s_sock < 0 || atomic_load(&g_aoip.ip) == 0) return;
    if (MC_HEADER_LEN + n > sizeof s_pkt) return;
    uint8_t *p = s_pkt;
    wr16(p + 0, start_code);
    wr16(p + 2, (uint16_t)(MC_HEADER_LEN + n));
    wr16(p + 4, s_seq);
    wr16(p + 6, g_aoip.process_id);
    memcpy(p + 8, g_aoip.devid, 8);
    memset(p + 16, ' ', 8);                  // vendor: [32; 8] then the string
    put_str(p, 16, 8, AOIP_VENDOR_STRING);
    memcpy(p + 24, opcode, 8);
    if (n) memcpy(p + MC_HEADER_LEN, content, n);
    s_seq = (uint16_t)(s_seq + 1u);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(dst_host),
    };
    sendto(s_sock, p, MC_HEADER_LEN + n, 0, (struct sockaddr *)&a, sizeof a);
}

static void send_info(const uint8_t opcode[8], const uint8_t *content, size_t n)
{
    mc_send(AOIP_MCAST_DEVICE_INFO, AOIP_PORT_INFO_DST, MC_START_CODE_INFO, opcode, content, n);
}

static void product_version(uint8_t out[4])
{
    // Inferno sends its crate version [major, minor, H(patch), L(patch)].
    unsigned maj = 0, min = 0, pat = 0;
    sscanf(UI_FW_VERSION, "%u.%u.%u", &maj, &min, &pat);
    out[0] = (uint8_t)maj;
    out[1] = (uint8_t)min;
    out[2] = (uint8_t)(pat >> 8);
    out[3] = (uint8_t)pat;
}

static void send_board_info(void)
{
    static uint8_t c[MC_BOARD_INFO_LEN];
    memset(c, 0, sizeof c);
    const uint8_t fw[4] = { 4, 1, 0, 6 }, hw[4] = { 4, 1, 0, 3 }, boot[4] = { 1, 0, 0, 0 };
    memcpy(c + 0, fw, 4);       // firmware version
    c[0x23] = 2;
    memcpy(c + 4, hw, 4);       // hardware version
    c[0x27] = 1;
    memcpy(c + 0x28, boot, 4);  // boot version
    // feature flags: 0x16 = 0x10 "has Manufacturer name"; nothing else
    // (no AES67, not lockable, no sample-rate/encoding configuration).
    c[0x14] = 0;
    c[0x15] = 0;
    c[0x16] = 0x10;
    c[0x17] = 0;
    c[0xbb] = 0x1f;             // "if 0, device is flooded with info multicast requests"
    put_str(c, 12, 8, AOIP_BOARD_NAME);
    put_str(c, 0x38, 16, AOIP_BOARD_NAME);
    const uint8_t op[8] = MC_OP_BOARD_INFO;
    send_info(op, c, sizeof c);
}

static void send_product_info(void)
{
    static uint8_t c[MC_PRODUCT_INFO_LEN];
    memset(c, 0, sizeof c);
    put_str(c, 0, 8, UI_MANUFACTURER);
    put_str(c, 8, 8, AOIP_BOARD_NAME);
    put_str(c, 0x2c, 16, UI_MANUFACTURER);
    put_str(c, 0xac, 16, UI_MODEL_NAME);
    product_version(c + 0x1c);
    const uint8_t op[8] = MC_OP_PRODUCT_INFO;
    send_info(op, c, sizeof c);
}

void info_send_board_and_product(void)
{
    send_board_info();
    send_product_info();
}

static void send_clock_stats(void)
{
    uint8_t id[8];
    mc_leader_id(id);
    static const uint8_t zero[8];
    // Inferno answers only when it knows the leader (clock-stats file found).
    if (memcmp(id, zero, 8) == 0) return;
    // mc_leader_id gives the PTPv1 sourceUuid (MAC) + port id; Inferno sends an
    // 8-byte clock id, which we take to be the EUI-64 of that MAC (unverified).
    const uint8_t leader[8] = { id[0], id[1], id[2], 0xff, 0xfe, id[3], id[4], id[5] };
    uint8_t c[8 + 4 + 6 + 2 + 8 + 8 + 76];
    bb_t bb;
    bb_init(&bb, c, sizeof c);
    const uint8_t head[8] = { 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x9f };
    bb_put(&bb, head, 8);
    bb_u32(&bb, (uint32_t)mc_freq_ppb());
    bb_put(&bb, g_aoip.mac, 6);
    bb_u16(&bb, 0);
    bb_put(&bb, leader, 8);
    bb_put(&bb, leader, 8);
    bb_zeros(&bb, 76);
    const uint8_t op[8] = MC_OP_CLOCK_STATS;
    send_info(op, c, bb.len);
}

static void send_network_info(void)
{
    uint8_t c[6 + 2 + 2 + 6 + 4 * 4 + 32];
    bb_t bb;
    bb_init(&bb, c, sizeof c);
    const uint8_t head[6] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
    bb_put(&bb, head, 6);
    bb_u16(&bb, 100);           // link speed, Mbit/s (10/100 MAC)
    bb_u16(&bb, 1);
    bb_put(&bb, g_aoip.mac, 6);
    uint32_t ip = atomic_load(&g_aoip.ip);
    bb_put(&bb, &ip, 4);        // network order in memory = wire order
    bb_put(&bb, &g_aoip.netmask, 4);
    bb_put(&bb, &g_aoip.gw, 4);
    bb_put(&bb, &g_aoip.gw, 4); // "DNS? doesn't really matter."
    const uint8_t tail[32] = { 0x00, 0x18, 0x00, 0x30 };
    bb_put(&bb, tail, 32);
    const uint8_t op[8] = MC_OP_NETWORK_INFO;
    send_info(op, c, bb.len);
}

void info_on_request(const uint8_t *pkt, size_t len)
{
    if (len < 32) return;
    const uint8_t *o = pkt + 24;
    if (o[0] != 0x07 || o[2] != 0 || o[4] != 0 || o[5] != 0 || o[6] != 0) return;
    switch (o[3]) {
    case MC_REQ_BOARD_INFO:
        if (o[7] == 0) send_board_info();
        break;
    case MC_REQ_PRODUCT_INFO:
        if (o[7] == 0) send_product_info();
        break;
    case MC_REQ_CLOCK_STATS:
        send_clock_stats();
        break;
    case MC_REQ_NETWORK_INFO:
        send_network_info();
        break;
    case MC_REQ_77: {
        const uint8_t op[8] = MC_OP_REPLY_78, content[8] = MC_REPLY_78_CONTENT;
        send_info(op, content, sizeof content);
        break;
    }
    default:
        break;
    }
}

void info_notify_rx_channels(uint32_t mask)
{
    if (s_notify_q) xQueueSend(s_notify_q, &mask, 0);
}

static void send_channel_change(uint32_t mask)
{
    // make_channel_change_notification: u16 mask length, then the bitmask
    // (bit i of byte i/8 = channel index i); at least one mask byte.
    uint8_t c[2 + 4] = { 0 };
    unsigned bytes = 1;
    for (unsigned i = 0; i < UI_CHANNELS; i++) {
        if (mask & (1u << i)) {
            c[2 + i / 8] |= (uint8_t)(1u << (i % 8));
            if (i / 8 + 1 > bytes) bytes = i / 8 + 1;
        }
    }
    wr16(c, (uint16_t)bytes);
    const uint8_t op[8] = MC_OP_CHANNEL_CHANGE;
    send_info(op, c, 2 + bytes);
}

void info_poll(void)
{
    uint32_t mask;
    while (s_notify_q && xQueueReceive(s_notify_q, &mask, 0) == pdTRUE) {
        send_channel_change(mask);
    }
}

static uint8_t peak_byte(uint32_t peak)
{
    // peaks.rs: round(-40 * log10(peak / i32::MAX)), clamped to 0..255
    if (peak == 0) return 255;
    float lin = (float)peak / 2147483647.0f;
    float v = roundf(-40.0f * log10f(lin));
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (uint8_t)v;
}

static void tx_peaks(uint32_t out[UI_CHANNELS])
{
    memset(out, 0, sizeof(uint32_t) * UI_CHANNELS);
#if CONFIG_USBINF_TEST_TONE
    for (unsigned c = 0; c < UI_CHANNELS; c++) out[c] = 214748364u;   // -20 dBFS
#elif CONFIG_USBINF_TEST_NET_LOOPBACK
    (void)out;  // filled from the RX peaks by the caller
#else
    // Sample the most recent half ring (5 ms) of the TX ring.
    audio_ring_t *r = &g_ring_usb_to_net;
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t now = mc_now_frames();
    if (head < RING_FRAMES / 2u || head + RING_FRAMES < now || head > now + RING_FRAMES) return;
    for (uint64_t f = head - RING_FRAMES / 2u; f < head; f++) {
        const int32_t *fr = ring_frame(r, f);
        for (unsigned c = 0; c < UI_CHANNELS; c++) {
            int32_t s = fr[c];
            uint32_t a = s < 0 ? (uint32_t)(-(int64_t)s) : (uint32_t)s;
            if (a > out[c]) out[c] = a;
        }
    }
#endif
}

void info_heartbeat(void)
{
    // 88 = 24 + 2 * 32: peaks block for the largest build (>= any UI_CHANNELS)
    static uint8_t c[16 + 88 + 24 + 4 * AOIP_RX_FLOW_SLOTS + 16];
    const uint16_t ctr = s_seq;
    bb_t bb;
    bb_init(&bb, c, sizeof c);

    // 0x8001: frequency offset (ppb). Our clock always exists (it free-runs
    // before PTP lock), so the block is always present.
    bb_u16(&bb, 16);
    bb_u16(&bb, 0x8001);
    bb_u16(&bb, 4);             // ???
    bb_u16(&bb, 4);             // maybe content length???
    bb_u16(&bb, ctr);
    bb_u16(&bb, 0);
    bb_u32(&bb, (uint32_t)mc_freq_ppb());

    // 0x8002: peaks, one byte per channel, TX first then RX
    uint32_t rxp[UI_CHANNELS], txp[UI_CHANNELS];
    frx_take_peaks(rxp);
    tx_peaks(txp);
#if CONFIG_USBINF_TEST_NET_LOOPBACK
    memcpy(txp, rxp, sizeof txp);
#endif
    const unsigned total_peaks = 2u * UI_CHANNELS;
    unsigned total_len = 24u + total_peaks;
    while (total_len % 4u) total_len++;
    uint16_t end_pos = (uint16_t)(bb.wpos + total_len);
    bb_u16(&bb, (uint16_t)(24u + total_peaks));
    bb_u16(&bb, 0x8002);
    bb_u16(&bb, 4);
    bb_u16(&bb, (uint16_t)(12u + total_peaks));
    bb_u16(&bb, ctr);
    bb_u16(&bb, 0);
    bb_u16(&bb, UI_CHANNELS);   // tx peaks count
    bb_u16(&bb, 0);
    bb_u16(&bb, UI_CHANNELS);   // rx peaks count
    bb_u16(&bb, 0);
    bb_u16(&bb, 24);
    bb_u16(&bb, 0);
    for (unsigned i = 0; i < UI_CHANNELS; i++) bb_u8(&bb, peak_byte(txp[i]));
    for (unsigned i = 0; i < UI_CHANNELS; i++) bb_u8(&bb, peak_byte(rxp[i]));
    while (!bb.ovf && bb.wpos < end_pos) bb_u8(&bb, 0);

    // 0x8003: receive latency per RX flow slot (all slots, unused = 0)
    const uint16_t nflows = AOIP_RX_FLOW_SLOTS;
    bb_u16(&bb, (uint16_t)(24u + nflows * 4u));
    bb_u16(&bb, 0x8003);
    bb_u16(&bb, 4);
    bb_u16(&bb, (uint16_t)(12u + nflows * 4u));
    bb_u16(&bb, ctr);
    bb_u16(&bb, 0);
    bb_u16(&bb, nflows);
    bb_u16(&bb, 0);
    bb_u16(&bb, 24);
    bb_u16(&bb, 0);
    bb_u32(&bb, UI_SAMPLE_RATE);
    for (unsigned i = 0; i < nflows; i++) {
        int32_t lat = frx_take_max_latency(i);
        bb_u32(&bb, (uint32_t)(lat < 0 ? 0 : lat));
    }

    if (bb.ovf) {
        ESP_LOGE(TAG, "heartbeat overflow");
        return;
    }
    const uint8_t op[8] = MC_OP_HEARTBEAT;
    mc_send(AOIP_MCAST_HEARTBEAT, AOIP_PORT_HEARTBEAT_DST, MC_START_CODE_HEARTBEAT, op, c, bb.len);
}
