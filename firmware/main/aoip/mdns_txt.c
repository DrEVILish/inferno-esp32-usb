// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-testable half of the mDNS TXT parsing (mdns_client.rs). See the
// header for the constraints that are checked before any narrowing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aoip/mdns_txt.h"

#define TXT_U16_MAX 0xFFFFu

// Find key in the list. A key without a value counts as missing: every
// consumer here reads numbers, and an empty string parses as nothing.
static const char *kv_get(const aoip_txt_kv_t *kv, size_t n, const char *key, char *buf, size_t cap)
{
    for (size_t i = 0; i < n; i++) {
        if (!kv[i].key || strcmp(kv[i].key, key) != 0) continue;
        if (!kv[i].value) return NULL;
        size_t len = kv[i].value_len < cap - 1u ? kv[i].value_len : cap - 1u;
        memcpy(buf, kv[i].value, len);
        buf[len] = 0;
        return buf;
    }
    return NULL;
}

// parse_int_from_dict: "0x" prefix = hex, else decimal. Values above
// 0xFFFF are refused before the caller narrows (all chan/bundle fields
// except latency_ns live in u16 slots).
static bool kv_int_u16(const aoip_txt_kv_t *kv, size_t n, const char *key, uint32_t *out)
{
    char buf[24];
    const char *s = kv_get(kv, n, key, buf, sizeof buf);
    if (!s || !s[0]) return false;
    char *end;
    unsigned long v = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? strtoul(s + 2, &end, 16)
                                                                   : strtoul(s, &end, 10);
    if (*end || v > TXT_U16_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

// latency_ns is a u32 field (a 10 s latency is legal); no 0x prefix in the
// protocol, parse strictly decimal.
static bool kv_int_u32(const aoip_txt_kv_t *kv, size_t n, const char *key, uint32_t *out)
{
    char buf[24];
    const char *s = kv_get(kv, n, key, buf, sizeof buf);
    if (!s || !s[0]) return false;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end) return false;
    *out = v;
    return true;
}

static bool parse_ipv4(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    uint8_t *p = (uint8_t *)out;
    p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = (uint8_t)c; p[3] = (uint8_t)d;
    return true;
}

bool mdns_txt_parse_chan(const aoip_txt_kv_t *kv, size_t n, aoip_adv_chan_t *o)
{
    memset(o, 0, sizeof *o);
    uint32_t v;
    if (!kv_int_u16(kv, n, "nchan", &v) || v == 0) return false;
    o->nchan = (uint16_t)v;
    // The channel id must name one of the channels the device advertised;
    // anything else is a malformed record, not a soft miss, because the
    // id feeds flow-update lists (a wrong id routes another channel).
    if (!kv_int_u16(kv, n, "id", &v) || v == 0 || v > o->nchan) return false;
    o->tx_channel_id = (uint16_t)v;
    if (!kv_int_u16(kv, n, "dbcp1", &v) || v == 0) return false;
    o->dbcp1 = (uint16_t)v;
    // The bit-depth key is "enc", older devices send "en"; either way it
    // must parse as a 16..32 byte-aligned depth (valued the same as the
    // subscriber's post-check).
    if (!kv_int_u16(kv, n, "enc", &v) && !kv_int_u16(kv, n, "en", &v)) return false;
    if (v < 16u || v > 32u || v % 8u != 0) return false;
    o->bits = v;
    char buf[24];
    const char *fpp = kv_get(kv, n, "fpp", buf, sizeof buf);
    unsigned fmax, fmin;
    if (!fpp || sscanf(fpp, "%u,%u", &fmax, &fmin) != 2) return false;
    if (fmax == 0 || fmax > TXT_U16_MAX || fmin == 0 || fmin > TXT_U16_MAX) return false;
    o->fpp_max = (uint16_t)fmax;
    o->fpp_min = (uint16_t)fmin;
    if (!kv_int_u32(kv, n, "latency_ns", &v)) return false;
    o->min_latency_ns = v;

    // Optional multicast bundle reference: keys "b.<id>" = channel in bundle+1
    o->has_bundle = false;
    for (size_t i = 0; i < n; i++) {
        const char *k = kv[i].key;
        if (!k || strncmp(k, "b.", 2) != 0) continue;
        char *end;
        unsigned long bid = strtoul(k + 2, &end, 10);
        if (k[2] == 0 || *end || bid == 0 || bid > TXT_U16_MAX) break;   // unparseable key
        char vb[12];
        const char *vs = kv_get(kv, n, k, vb, sizeof vb);
        if (!vs) break;
        unsigned long chb = strtoul(vs, &end, 10);
        if (*end || chb == 0 || chb > TXT_U16_MAX) break;
        o->has_bundle = true;
        o->bundle_id = (uint16_t)bid;
        o->channel_in_bundle = (uint16_t)(chb - 1u);
        break;
    }
    return true;
}

bool mdns_txt_parse_bundle(const aoip_txt_kv_t *kv, size_t n, aoip_adv_bundle_t *o)
{
    memset(o, 0, sizeof *o);
    char buf[24];
    const char *a = kv_get(kv, n, "a.0", buf, sizeof buf);
    if (!a || !parse_ipv4(a, &o->ip)) return false;
    uint32_t v;
    if (!kv_int_u16(kv, n, "p.0", &v) || v == 0) return false;
    o->port = (uint16_t)v;
    if (!kv_int_u16(kv, n, "nchan", &v) || v == 0) return false;
    o->nchan = (uint16_t)v;
    if (!kv_int_u16(kv, n, "id", &v) || v == 0) return false;
    o->bundle_id = (uint16_t)v;
    if (!kv_int_u16(kv, n, "enc", &v) && !kv_int_u16(kv, n, "en", &v)) return false;
    if (v < 16u || v > 32u || v % 8u != 0) return false;
    o->bits = v;
    if (!kv_int_u16(kv, n, "fpp", &v) || v == 0) return false;
    o->fpp = (uint16_t)v;
    if (!kv_int_u32(kv, n, "latency_ns", &v)) return false;
    o->min_latency_ns = v;
    return true;
}
