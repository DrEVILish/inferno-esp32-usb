// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the fork-comparable AoIP replies, using the REAL firmware
// sources (firmware/main/aoip/arc.c, wire.c, info_mcast.c) with stubbed
// IDF/socket headers (tools/host_stub) and fake device state
// (tools/aoip_host_stubs.c).
//
//   tools/aoip_host_test.sh            build + run, asserts
//   DUMP=1 tools/aoip_host_test.sh    print reply hex for tools/check_vectors.py,
//                                     which compares against the fork sources
//                                     in /tmp/drevinferno (ground truth)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"
#include "aoip/wire.h"

// aoip_host_stubs.c capture interface
void host_cap_reset(void);
unsigned host_cap_count(void);
const uint8_t *host_cap_pkt(unsigned i, size_t *len_out);
const uint8_t *host_cap_op(unsigned i);
extern uint8_t host_leader_id[8];
extern int32_t host_freq_ppb;

static int failures;
static int dumping;

static void hexline(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x%s", p[i], i + 1 < n ? " " : "\n");
}

static void check_blob(const char *label, const uint8_t *got, size_t got_n,
                       const uint8_t *want, size_t want_n)
{
    if (dumping) {
        printf("%s:", label);
        hexline(got, got_n);
        return;
    }
    if (got_n != want_n || memcmp(got, want, want_n) != 0) {
        printf("FAIL %s: got %u bytes, want %u bytes\n", label, (unsigned)got_n, (unsigned)want_n);
        printf("  got : ");
        hexline(got, got_n);
        printf("  want: ");
        hexline(want, want_n);
        failures++;
    } else {
        printf("ok %s (%u bytes)\n", label, (unsigned)got_n);
    }
}

// Build an ARC request: header + content. Returns total length.
static size_t mkreq(uint8_t *pkt, uint16_t op, const uint8_t *content, size_t n)
{
    rr_header(pkt, 0x1234, (uint16_t)(RR_HEADER_LEN + n), 7, op, 0);
    if (n) memcpy(pkt + RR_HEADER_LEN, content, n);
    return RR_HEADER_LEN + n;
}

static void test_2204(void)
{
    uint8_t req[32], out[256];
    // fork-observed first-page query content
    const uint8_t q[] = { 0x00, 0x01, 0x00, 0x01, 0x00, 0x00 };
    size_t rl = mkreq(req, 0x2204, q, sizeof q);
    size_t n = arc_handle(req, rl, out, sizeof out);
    const uint8_t want[] = {
        0x12, 0x34, 0x00, 0x0c, 0x00, 0x07, 0x22, 0x04, 0x00, 0x01,
        0x02, 0x00,
    };
    check_blob("arc-2204", out, n, want, sizeof want);
}

static void test_1100(void)
{
    uint8_t req[32], out[256];
    size_t rl = mkreq(req, ARC_OP_DEVICE_SETTINGS, 0, 0);
    size_t n = arc_handle(req, rl, out, sizeof out);
    // profile 10: latency 48 frames = 1 000 000 ns = 0x000F4240
    const uint8_t want[] = {
        0x12, 0x34, 0x00, 0x3c, 0x00, 0x07, 0x11, 0x00, 0x00, 0x01,
        0x02, 0x06,
        0x80, 0x20, 0x00, 0x24, 0x82, 0x04, 0x00, 0x28,
        0x82, 0x05, 0x00, 0x2c, 0x83, 0x01, 0x00, 0x30,
        0x83, 0x02, 0x00, 0x34, 0x83, 0x06, 0x00, 0x38,
        0x00, 0x00, 0xbb, 0x80,
        0x00, 0x0f, 0x42, 0x40, 0x00, 0x0f, 0x42, 0x40,
        0x00, 0x0f, 0x42, 0x40, 0x00, 0x98, 0x96, 0x80,
        0x00, 0x07, 0xa1, 0x20,
    };
    check_blob("arc-1100", out, n, want, sizeof want);
    if (!dumping) {
        // structural: each record offset (minus header) points at its value
        const uint8_t *c = out + RR_HEADER_LEN;
        for (unsigned i = 0; i < 6; i++) {
            unsigned off = ((unsigned)c[2 + i * 4 + 2] << 8 | c[2 + i * 4 + 3]) - RR_HEADER_LEN;
            uint32_t v = (uint32_t)c[off] << 24 | (uint32_t)c[off + 1] << 16 |
                         (uint32_t)c[off + 2] << 8 | c[off + 3];
            uint32_t expect[6] = { 48000u, 1000000u, 1000000u, 1000000u, 10000000u, 500000u };
            if (v != expect[i]) {
                printf("FAIL arc-1100 rec %u offset points at %lu\n", i, (unsigned long)v);
                failures++;
            }
        }
    }
}

static void test_1102(void)
{
    uint8_t req[32], out[256];
    size_t rl = mkreq(req, ARC_OP_PROPERTY_DIRECTORY, 0, 0);
    size_t n = arc_handle(req, rl, out, sizeof out);
    static const uint16_t pairs[31][2] = {
        { 0x8020u, 1 }, { 0x8021u, 3 }, { 0x0022u, 3 }, { 0x0023u, 3 },
        { 0x0024u, 1 }, { 0x8060u, 3 }, { 0x0062u, 3 }, { 0x0063u, 1 },
        { 0x0201u, 3 }, { 0x8204u, 3 }, { 0x8205u, 3 }, { 0x020au, 1 },
        { 0x020bu, 1 }, { 0x0210u, 3 }, { 0x0211u, 3 }, { 0x0212u, 3 },
        { 0x0213u, 1 }, { 0x0214u, 1 }, { 0x0222u, 3 }, { 0x8301u, 3 },
        { 0x8306u, 1 }, { 0x8302u, 1 }, { 0x8321u, 1 }, { 0x0310u, 1 },
        { 0x0311u, 1 }, { 0x0312u, 1 }, { 0x0303u, 3 }, { 0x83f0u, 1 },
        { 0x0601u, 1 }, { 0x0309u, 1 }, { 0x0209u, 1 },
    };
    uint8_t want[10 + 2 + 31 * 4];
    rr_header(want, 0x1234, (uint16_t)sizeof want, 7, 0x1102, 1);
    want[10] = 0x00;
    want[11] = 0x1f;
    for (unsigned i = 0; i < 31; i++) {
        want[12 + i * 4 + 0] = (uint8_t)(pairs[i][0] >> 8);
        want[12 + i * 4 + 1] = (uint8_t)pairs[i][0];
        want[12 + i * 4 + 2] = (uint8_t)(pairs[i][1] >> 8);
        want[12 + i * 4 + 3] = (uint8_t)pairs[i][1];
    }
    check_blob("arc-1102", out, n, want, sizeof want);
}

static void test_1101(void)
{
    uint8_t req[64], out[64];
    // fork-style latency write asking for 2 ms via 0x8205
    const uint8_t q[] = {
        0x05, 0x04, 0x82, 0x05, 0x00, 0x20, 0x02, 0x11, 0x00, 0x04,
        0x83, 0x01, 0x00, 0x24, 0x03, 0x10, 0x00, 0x04, 0x83, 0x02,
        0x83, 0x06, 0x01, 0xe8, 0x48, 0x00, 0x01, 0xe8, 0x48, 0x00,
    };
    size_t rl = mkreq(req, ARC_OP_DEVICE_SETTINGS_WRITE, q, sizeof q);
    size_t n = arc_handle(req, rl, out, sizeof out);
    // clean refusal: empty body, code 0x30 (no controller timeout)
    const uint8_t want[] = {
        0x12, 0x34, 0x00, 0x0a, 0x00, 0x07, 0x11, 0x01, 0x00, 0x30,
    };
    check_blob("arc-1101", out, n, want, sizeof want);
}

static void test_unknown(void)
{
    uint8_t req[32], out[64];
    size_t rl = mkreq(req, 0x9999, 0, 0);
    size_t n = arc_handle(req, rl, out, sizeof out);
    if (!dumping && n != 0) {
        printf("FAIL arc-unknown: got %u bytes, want 0\n", (unsigned)n);
        failures++;
    } else if (!dumping) {
        printf("ok arc-unknown (no reply)\n");
    }
}

// Multicast probe: 32 B packet, opcode at bytes 24..31.
static void mkprobe(uint8_t *pkt, uint8_t b3)
{
    memset(pkt, 0, 32);
    pkt[24] = 0x07;
    pkt[26] = 0x00;
    pkt[27] = b3;
    pkt[28] = 0x00;
    pkt[29] = 0x00;
    pkt[30] = 0x00;
}

static void setup_device(void)
{
    const uint8_t mac[6] = { 0xd8, 0x3a, 0xdd, 0x80, 0x89, 0xe9 };
    memcpy(g_aoip.mac, mac, 6);
    const uint8_t devid[8] = { 0xd8, 0x3a, 0xdd, 0xff, 0xfe, 0x80, 0x89, 0xe9 };
    memcpy(g_aoip.devid, devid, 8);
    snprintf(g_aoip.friendly_hostname, sizeof g_aoip.friendly_hostname, "INFUSB-89E9");
    snprintf(g_aoip.factory_hostname, sizeof g_aoip.factory_hostname, "planes-89e9");
    g_aoip.process_id = 0;
    atomic_store(&g_aoip.ip, 0x0101A8C0u);
    info_set_socket(1);
}

static void test_capability(void)
{
    host_cap_reset();
    info_send_board_and_product();
    if (!dumping && host_cap_count() != 4) {
        printf("FAIL info-startup: %u packets, want 4\n", host_cap_count());
        failures++;
        return;
    }
    // packets 2,3 are the capability statuses (board, product first)
    size_t n0, n1;
    const uint8_t *p80 = host_cap_pkt(2, &n0);
    const uint8_t *p82 = host_cap_pkt(3, &n1);
    const uint8_t want80[] = {
        0x00, 0x18, 0x00, 0x01, 0x00, 0x00, 0xbb, 0x80,
        0x00, 0x00, 0xbb, 0x80, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xbb, 0x80,
    };
    const uint8_t want82[] = {
        0x00, 0x18, 0x00, 0x01, 0x00, 0x00, 0x00, 0x18,
        0x00, 0x00, 0x00, 0x18, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x18,
    };
    check_blob("info-cap80", p80 + MC_HEADER_LEN, n0 - MC_HEADER_LEN, want80, sizeof want80);
    check_blob("info-cap82", p82 + MC_HEADER_LEN, n1 - MC_HEADER_LEN, want82, sizeof want82);
    if (!dumping) {
        // framing: start 0xffff, opcode bytes, total length
        if (p80[0] != 0xff || p80[1] != 0xff || p80[24] != 0x07 || p80[25] != 0x24 ||
            p80[26] != 0x00 || p80[27] != 0x80) {
            printf("FAIL info-cap80 framing\n");
            failures++;
        } else {
            printf("ok info-cap80 framing\n");
        }
    }
    // probes 0x81 / 0x83 re-announce
    uint8_t probe[32];
    host_cap_reset();
    mkprobe(probe, 0x81);
    info_on_request(probe, sizeof probe);
    if (!dumping) {
        if (host_cap_count() != 1) {
            printf("FAIL info-probe81: %u packets\n", host_cap_count());
            failures++;
        } else {
            size_t n;
            const uint8_t *p = host_cap_pkt(0, &n);
            check_blob("info-probe81", p + MC_HEADER_LEN, n - MC_HEADER_LEN, want80, sizeof want80);
        }
    }
    host_cap_reset();
    mkprobe(probe, 0x83);
    info_on_request(probe, sizeof probe);
    if (!dumping) {
        if (host_cap_count() != 1) {
            printf("FAIL info-probe83: %u packets\n", host_cap_count());
            failures++;
        } else {
            size_t n;
            const uint8_t *p = host_cap_pkt(0, &n);
            check_blob("info-probe83", p + MC_HEADER_LEN, n - MC_HEADER_LEN, want82, sizeof want82);
        }
    }
}

static void test_clock(void)
{
    const uint8_t leader[8] = { 0x00, 0x1d, 0xc1, 0x52, 0x17, 0xc7, 0x00, 0x00 };
    memcpy(host_leader_id, leader, 8);
    host_freq_ppb = -13204;
    uint8_t probe[32];
    host_cap_reset();
    mkprobe(probe, 0x21);
    info_on_request(probe, sizeof probe);
    if (!dumping && host_cap_count() != 1) {
        printf("FAIL info-clock: %u packets, want 1\n", host_cap_count());
        failures++;
        return;
    }
    size_t n;
    const uint8_t *p = host_cap_pkt(0, &n);
    const uint8_t *c = p + MC_HEADER_LEN;
    size_t cn = n - MC_HEADER_LEN;
    // fork build_clock_status(own=d8:3a:dd:80:89:e9, leader, -13204)
    // = CLOCK_STATUS_TEMPLATE with sync 3, slave states, ppb, MAC, leader,
    // EUI-64 identities (verified against the fork source by check_vectors.py)
    static const uint8_t want[188] = {
        0x00, 0x03, 0x00, 0x09, 0x00, 0x00, 0x00, 0x9f,
        0xff, 0xff, 0xcc, 0x6c, 0xd8, 0x3a, 0xdd, 0x80,
        0x89, 0xe9, 0x00, 0x00, 0x00, 0x1d, 0xc1, 0x52,
        0x17, 0xc7, 0x00, 0x00, 0x00, 0x1d, 0xc1, 0x52,
        0x17, 0xc7, 0x00, 0x00, 0x00, 0x01, 0x00, 0x34,
        0x00, 0x09, 0x00, 0x00, 0x02, 0x94, 0x00, 0x00,
        0x00, 0x03, 0x0d, 0x40, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x60, 0x0c, 0x00,
        0x00, 0x00, 0x00, 0x0c, 0x00, 0x98, 0x00, 0x20,
        0x00, 0x03, 0x00, 0x00, 0x00, 0x68, 0x10, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x09, 0x00, 0x07,
        0x00, 0x01, 0x00, 0x02, 0x02, 0x02, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x03,
        0x00, 0x01, 0x00, 0x03, 0x02, 0x02, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x03, 0x00, 0x07,
        0x00, 0x03, 0x00, 0x07, 0x00, 0xb8, 0x00, 0x04,
        0xd8, 0x3a, 0xdd, 0xff, 0xfe, 0x80, 0x89, 0xe9,
        0x00, 0x1d, 0xc1, 0xff, 0xfe, 0x52, 0x17, 0xc7,
        0x00, 0x1d, 0xc1, 0xff, 0xfe, 0x52, 0x17, 0xc7,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00,
    };
    check_blob("info-clock", c, cn, want, sizeof want);
    // gating: unknown leader -> silence
    memset(host_leader_id, 0, 8);
    host_cap_reset();
    info_on_request(probe, sizeof probe);
    if (!dumping && host_cap_count() != 0) {
        printf("FAIL info-clock-gated: %u packets\n", host_cap_count());
        failures++;
    } else if (!dumping) {
        printf("ok info-clock-gated (silent)\n");
    }
}

int main(void)
{
    dumping = getenv("DUMP") != 0;
    test_2204();
    test_1100();
    test_1102();
    test_1101();
    test_unknown();
    setup_device();
    test_capability();
    test_clock();
    if (!dumping) {
        if (failures == 0) printf("arc_info_host_test: all pass\n");
        return failures != 0;
    }
    return 0;
}
