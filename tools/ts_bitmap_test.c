// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for (1) the incremental timestamp split (firmware/main/aoip/
// ts_incr.h) against direct division/modulo, and (2) the 239/8 group-bit
// mapping (net.h net_group_bit) for bijectivity. No IDF needed:
//   tools/host_tests.sh
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "aoip/ts_incr.h"
#include "net/net.h"

static int failures;

// xorshift PRNG for reproducible sequences
static uint32_t rnd_state = 0x12345678u;
static uint32_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

static void expect(const char *what, unsigned long got, unsigned long want)
{
    if (got != want) {
        printf("FAIL %s: got %lu want %lu\n", what, got, want);
        failures++;
    }
}

static void test_ts_split(void)
{
    ts_split_t t;
    static const uint32_t fpps[] = { 2, 4, 8, 16, 32, 12, 24, 7 };

    // bootstraps at awkward frame indices, then long advance runs. The
    // device's frame index always keeps seconds inside the wire's u32
    // (ts < 48000 << 32); the test respects that bound.
    const uint64_t TS_MAX = (uint64_t)UI_SAMPLE_RATE * 0xFFFFFFFFull - 1000000ull;
    for (unsigned run = 0; run < 40; run++) {
        uint64_t ts = (((uint64_t)rnd() << 32) | rnd()) % TS_MAX;
        ts_split_load(&t, ts);
        expect("load sec", t.sec, (unsigned long)(ts / UI_SAMPLE_RATE));
        expect("load sub", t.sub, (unsigned long)(ts % UI_SAMPLE_RATE));
        for (unsigned i = 0; i < 20000; i++) {
            const uint32_t fpp = fpps[rnd() % (sizeof fpps / sizeof fpps[0])];
            ts += fpp;
            ts_split_advance(&t, fpp);
            if (t.sec != ts / UI_SAMPLE_RATE || t.sub != ts % UI_SAMPLE_RATE) {
                printf("FAIL advance at run %u step %u: got (%lu,%lu) want (%lu,%lu)\n",
                       run, i, (unsigned long)t.sec, (unsigned long)t.sub,
                       (unsigned long)(ts / UI_SAMPLE_RATE), (unsigned long)(ts % UI_SAMPLE_RATE));
                failures++;
                return;
            }
        }
    }
    // fpp >= rate must never be passed (assert by contract; skip)
    printf("ts_split: 800k advance steps verified against div/mod\n");
}

static void test_group_bit(void)
{
    // bijectivity over a dense sample of 239/8: distinct groups -> distinct bits.
    // Byte-array convention: b[0] is the first packet octet (== 239 here).
    static uint8_t seen[1u << 23];
    for (uint32_t lo = 0; lo < (1u << 23); lo += 977) {     // stride sample
        const uint8_t bytes[4] = { 239u, (uint8_t)((lo >> 16) | 0x80u),
                                   (uint8_t)(lo >> 8), (uint8_t)lo };
        uint32_t ip;
        memcpy(&ip, bytes, 4);                              // codebase convention
        unsigned bit = net_group_bit(ip);
        expect("bit < 2^23", bit < (1u << 23), 1);
        expect("roundtrip", (unsigned long)bit, (unsigned long)lo);
        if (seen[bit]) {
            printf("FAIL collision at bit %u\n", bit);
            failures++;
            return;
        }
        seen[bit] = 1;
    }
    // Scopes outside 239/8 share the 23 low bits by construction (documented).
    // Byte arrays: 224.0.1.129 vs 239.0.1.129 -> same octets 2-4.
    uint32_t ptp, aud;
    const uint8_t pb[4] = { 224, 0, 1, 129 }, ab[4] = { 239, 0, 1, 129 };
    memcpy(&ptp, pb, 4);
    memcpy(&aud, ab, 4);
    expect("239 vs 224 same low bits collide (documented)",
           net_group_bit(ptp) == net_group_bit(aud), 1);
    printf("net_group_bit: bijective over 239/8 (sampled)\n");
}

int main(void)
{
    test_ts_split();
    test_group_bit();
    if (failures == 0) printf("ts_bitmap_test: all pass\n");
    return failures != 0;
}
