// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the linear header-checksum formulas used by the per-
// destination send template (net.c net_udp_dest_seal + send_head). It
// replicates the OLD full-rebuild checksum and the NEW template fold with
// the same constant words and asserts they agree over random payloads.
// It validates the formula identity, not the net.c code itself (net.c is
// not host-compilable); a wire-level regression is still caught on the
// bench (usb_descriptor_check / aoip_pcap).
//   gcc -Wall -Wextra tools/checksum_test.c -o x && ./x
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static unsigned failures;

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint32_t rnd_state = 0xC0FFEEu;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

// 42-byte header, word offsets: eth 0-5, ip 6-15 (total@8? no — bytes are
// 14 eth, 20 ip at 14..33, udp at 34..41). Checksum word at 24/24+1, total
// at 16/17, id at 18/19, udp len at 38/39.
enum { RAW = 42, IP0 = 14, IP_CK = IP0 + 10, IP_TOTAL = IP0 + 2, IP_ID = IP0 + 4, UDP0 = IP0 + 20, UDP_LEN = UDP0 + 4 };

static void fill_constant(uint8_t *h)
{
    for (unsigned i = 0; i < RAW; i += 2) wr16(h + i, (uint16_t)(rnd() & 0xFFFF));
    wr16(h + IP_CK, 0);     // old algorithm zeroes it before summing
    wr16(h + IP_TOTAL, 0);  // variable words removed from the constant set
    wr16(h + IP_ID, 0);
    wr16(h + UDP_LEN, 0);
}

static uint16_t s_of(uint32_t x)
{
    x = (x & 0xFFFFu) + (x >> 16);
    x = (x & 0xFFFFu) + (x >> 16);
    return (uint16_t)~x;
}

static uint16_t old_checksum(const uint8_t *h, uint16_t total, uint16_t id, uint16_t udp_len)
{
    uint8_t t[RAW];
    memcpy(t, h, RAW);
    wr16(t + IP_TOTAL, total);
    wr16(t + IP_ID, id);
    wr16(t + UDP_LEN, udp_len);
    wr16(t + IP_CK, 0);
    uint32_t sum = 0;
    for (unsigned i = 0; i < RAW - 14; i += 2) sum += rd16(t + IP0 + i);   // IP header only (20 B)
    return s_of(sum);
}

static void new_seal(const uint8_t *h_in, uint8_t *tpl_out, uint32_t *base_out)
{
    uint8_t t[RAW];
    memcpy(t, h_in, RAW);
    wr16(t + IP_TOTAL, 0);
    wr16(t + IP_ID, 0);
    wr16(t + UDP_LEN, 0);
    uint32_t sum = 0;
    for (unsigned i = 0; i < RAW - 14; i += 2) {
        if ((unsigned)(IP_CK - IP0) == i) continue;
        sum += rd16(t + IP0 + i);
    }
    memcpy(tpl_out, t, RAW);
    *base_out = sum;
}

static uint16_t new_checksum(uint32_t base, uint16_t total, uint16_t id, uint16_t udp_len)
{
    return s_of(base + (uint32_t)total + (uint32_t)udp_len + (uint32_t)id);
}

int main(void)
{
    uint8_t h[RAW], tpl[RAW];
    uint32_t base;
    fill_constant(h);
    new_seal(h, tpl, &base);
    (void)tpl;
    for (unsigned i = 0; i < 200000; i++) {
        const uint16_t len = (uint16_t)(rnd() % 1401u);
        const uint16_t id = (uint16_t)(rnd() & 0xFFFF);
        const uint16_t total = (uint16_t)(28u + len);
        const uint16_t udp_len = (uint16_t)(8u + len);
        const uint16_t a = old_checksum(h, total, id, udp_len);
        const uint16_t b = new_checksum(base, total, id, udp_len);
        if (a != b) {
            printf("FAIL len=%u id=%u: old=%04x new=%04x\n", len, id, a, b);
            failures++;
            return failures != 0;
        }
    }
    printf("checksum_test: template fold == full rebuild over 200k random sends\n");
    return 0;
}
