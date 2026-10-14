// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the TXT parser split (firmware/main/aoip/mdns_txt.c):
// oversized and malformed discovery values must be refused BEFORE being
// narrowed to a 16-bit field — id=65537 used to wrap to channel 1 and be
// subscribed as a channel the transmitter never advertised.
//   tools/host_tests.sh (+ mdns_txt_test)
#include <stdio.h>
#include <string.h>

#include "aoip/mdns_txt.h"

static int s_fail;

#define OK(expr) do { \
        if (!(expr)) { s_fail++; printf("FAIL line %d: %s\n", __LINE__, #expr); } \
    } while (0)

typedef aoip_txt_kv_t kv_t;

// A record is a NULL-terminated table of {key, value} rows; a value of "!"
// stands for a valueless key (mDNS "default"-style entry).
typedef const char *row_t[2];

static size_t view(kv_t *kv, size_t cap, const row_t *rows)
{
    size_t n = 0;
    for (size_t i = 0; rows[i][0]; i++) {
        if (n >= cap) break;
        const bool valueless = rows[i][1][0] == '!' && rows[i][1][1] == 0;
        kv[n].key = rows[i][0];
        kv[n].value = valueless ? NULL : rows[i][1];
        kv[n].value_len = valueless ? 0 : strlen(rows[i][1]);
        n++;
    }
    return n;
}

static bool chan_ok(const row_t *rows, aoip_adv_chan_t *o)
{
    kv_t kv[16];
    aoip_adv_chan_t ch;
    memset(&ch, 0xEE, sizeof ch);
    const size_t n = view(kv, 16, rows);
    const bool ok = mdns_txt_parse_chan(kv, n, &ch);
    if (o) *o = ch;
    return ok;
}

static bool bundle_ok(const row_t *rows, aoip_adv_bundle_t *o)
{
    kv_t kv[8];
    const size_t n = view(kv, 8, rows);
    return mdns_txt_parse_bundle(kv, n, o);
}

#define CHAN_BASE { "dbcp1", "0x1102" }, { "enc", "24" }, { "fpp", "32,2" }, { "latency_ns", "1000" }

int main(void)
{
    // Fully valid record (matches what we publish), bundle key included.
    static const row_t good[] = {
        { "txtvers", "2" }, { "dbcp1", "0x1102" }, { "id", "3" }, { "nchan", "8" },
        { "rate", "48000" }, { "pcm", "3 e" }, { "enc", "24" }, { "en", "24" },
        { "latency_ns", "1000000" }, { "fpp", "32,2" }, { "b.1", "5" }, { NULL, NULL },
    };
    aoip_adv_chan_t ch;
    OK(chan_ok(good, &ch));
    OK(ch.nchan == 8);
    OK(ch.tx_channel_id == 3);
    OK(ch.dbcp1 == 0x1102);
    OK(ch.bits == 24);
    OK(ch.fpp_max == 32 && ch.fpp_min == 2);
    OK(ch.min_latency_ns == 1000000);
    OK(ch.has_bundle && ch.bundle_id == 1 && ch.channel_in_bundle == 4);

    // id must name one of the advertised channels: 0, out of range, and the
    // wrap case (id=65537 -> 1) are all refused before narrowing.
    {
        static const row_t t[] = { { "nchan", "8" }, { "id", "0" }, CHAN_BASE, { NULL, NULL } };
        OK(!chan_ok(t, NULL));
        static const row_t t2[] = { { "nchan", "8" }, { "id", "9" }, CHAN_BASE, { NULL, NULL } };
        OK(!chan_ok(t2, NULL));
        static const row_t t3[] = { { "nchan", "8" }, { "id", "65537" }, CHAN_BASE, { NULL, NULL } };
        OK(!chan_ok(t3, NULL));
        static const row_t t4[] = { { "nchan", "65537" }, { "id", "1" }, CHAN_BASE, { NULL, NULL } };
        OK(!chan_ok(t4, NULL));
        static const row_t t5[] = { { "nchan", "0" }, { "id", "1" }, CHAN_BASE, { NULL, NULL } };
        OK(!chan_ok(t5, NULL));
    }

    // Depth: the old "en" fallback still accepted; malformed depths refused.
    {
        static const row_t t[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                   { "en", "16" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                   { NULL, NULL } };
        OK(chan_ok(t, NULL));
        static const row_t t2[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "33" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                    { NULL, NULL } };
        OK(!chan_ok(t2, NULL));
        static const row_t t3[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "fpp", "32,2" }, { "latency_ns", "1000" }, { NULL, NULL } };
        OK(!chan_ok(t3, NULL));     // no depth key at all
        static const row_t t4[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "16" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                    { NULL, NULL } };
        OK(chan_ok(t4, NULL));
    }

    // dbcp1 nonzero; fpp pair nonzero and 16-bit.
    {
        static const row_t t2[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0" },
                                    { "enc", "24" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                    { NULL, NULL } };
        OK(!chan_ok(t2, NULL));
        static const row_t t3[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "0,2" }, { "latency_ns", "1000" },
                                    { NULL, NULL } };
        OK(!chan_ok(t3, NULL));
        static const row_t t4[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "32,0" }, { "latency_ns", "1000" },
                                    { NULL, NULL } };
        OK(!chan_ok(t4, NULL));
        static const row_t t5[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "70000,2" }, { "latency_ns", "1000" },
                                    { NULL, NULL } };
        OK(!chan_ok(t5, NULL));
        static const row_t t6[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "32,700000" }, { "latency_ns", "1000" },
                                    { NULL, NULL } };
        OK(!chan_ok(t6, NULL));
    }

    // latency_ns is a u32 field (no wrap risk), still required.
    {
        static const row_t t[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                   { "enc", "24" }, { "fpp", "32,2" }, { NULL, NULL } };
        OK(!chan_ok(t, NULL));
        static const row_t t2[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "32,2" },
                                    { "latency_ns", "4294967295" }, { NULL, NULL } };
        OK(chan_ok(t2, NULL));
    }

    // Bundle reference keys: bad value/id keys are skipped (has_bundle
    // false), the channel itself still parses.
    {
        static const row_t t[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                   { "enc", "24" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                   { "b.1", "0" }, { NULL, NULL } };
        OK(chan_ok(t, &ch));
        OK(!ch.has_bundle);
        static const row_t t2[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                    { "b.65536", "1" }, { NULL, NULL } };
        OK(chan_ok(t2, &ch));
        OK(!ch.has_bundle);
        static const row_t t3[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                    { "b.1", "65537" }, { NULL, NULL } };
        OK(chan_ok(t3, &ch));
        OK(!ch.has_bundle);
        static const row_t t4[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                    { "b.x", "1" }, { NULL, NULL } };
        OK(chan_ok(t4, &ch));
        OK(!ch.has_bundle);
        static const row_t t5[] = { { "nchan", "8" }, { "id", "1" }, { "dbcp1", "0x1102" },
                                    { "enc", "24" }, { "fpp", "32,2" }, { "latency_ns", "1000" },
                                    { "b.1", "2" }, { NULL, NULL } };
        OK(chan_ok(t5, &ch));
        OK(ch.has_bundle && ch.bundle_id == 1 && ch.channel_in_bundle == 1);
    }

    // Bundle records.
    {
        static const row_t bgood[] = { { "a.0", "239.255.1.2" }, { "p.0", "4321" },
                                       { "nchan", "4" }, { "id", "2" }, { "fpp", "32" },
                                       { "enc", "16" }, { "latency_ns", "2000000" },
                                       { NULL, NULL } };
        aoip_adv_bundle_t bu;
        memset(&bu, 0xEE, sizeof bu);
        OK(bundle_ok(bgood, &bu));
        // p[0]=239 p[1]=255 p[2]=1 p[3]=2, little-endian u32
        OK(bu.ip == 0x0201FFEFu);
        OK(bu.port == 4321 && bu.nchan == 4 && bu.bundle_id == 2 && bu.fpp == 32 && bu.bits == 16);
        OK(bu.min_latency_ns == 2000000);

        static const row_t t[] = { { "a.0", "0" }, { "p.0", "4321" }, { "nchan", "4" },
                                   { "id", "2" }, { "fpp", "32" }, { "enc", "16" },
                                   { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t, &bu));
        static const row_t t2[] = { { "a.0", "400.1.1.1" }, { "p.0", "4321" }, { "nchan", "4" },
                                    { "id", "2" }, { "fpp", "32" }, { "enc", "16" },
                                    { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t2, &bu));
        static const row_t t3[] = { { "a.0", "1.2.3.4x" }, { "p.0", "4321" }, { "nchan", "4" },
                                    { "id", "2" }, { "fpp", "32" }, { "enc", "16" },
                                    { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t3, &bu));
        static const row_t t4[] = { { "a.0", "239.255.1.2" }, { "p.0", "0" }, { "nchan", "4" },
                                    { "id", "2" }, { "fpp", "32" }, { "enc", "16" },
                                    { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t4, &bu));
        static const row_t t5[] = { { "a.0", "239.255.1.2" }, { "p.0", "99999" }, { "nchan", "4" },
                                    { "id", "2" }, { "fpp", "32" }, { "enc", "16" },
                                    { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t5, &bu));
        static const row_t t6[] = { { "a.0", "239.255.1.2" }, { "p.0", "4321" }, { "nchan", "0" },
                                    { "id", "2" }, { "fpp", "32" }, { "enc", "16" },
                                    { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t6, &bu));
        static const row_t t7[] = { { "a.0", "239.255.1.2" }, { "p.0", "4321" }, { "nchan", "4" },
                                    { "id", "0" }, { "fpp", "32" }, { "enc", "16" },
                                    { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t7, &bu));
        static const row_t t8[] = { { "a.0", "239.255.1.2" }, { "p.0", "4321" }, { "nchan", "4" },
                                    { "id", "2" }, { "fpp", "0" }, { "enc", "16" },
                                    { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t8, &bu));
        static const row_t t9[] = { { "a.0", "239.255.1.2" }, { "p.0", "4321" }, { "nchan", "4" },
                                    { "id", "2" }, { "fpp", "32" }, { "enc", "!" },
                                    { "latency_ns", "1" }, { NULL, NULL } };
        OK(!bundle_ok(t9, &bu));    // valueless key counts as missing
    }

    if (s_fail) {
        printf("mdns_txt_test: %d FAILURES\n", s_fail);
        return 1;
    }
    printf("mdns_txt_test: all pass\n");
    return 0;
}
