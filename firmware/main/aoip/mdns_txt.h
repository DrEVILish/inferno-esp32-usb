// SPDX-License-Identifier: GPL-3.0-or-later
//
// TXT parsing of the _netaudio-chan / _netaudio-bund mDNS advertisements
// (Inferno mdns_client.rs "ChanTxt"/"BundleTxt"). Split out of aoip_mdns.c
// so it is host-testable (tools/mdns_txt_test.c): the caller adapts
// espressif/mdns results into key/value lists, everything here is ANSI C.
//
// Every integer is range-checked against its protocol limit BEFORE it is
// narrowed to a 16-bit struct field. A value like id=65537 used to wrap
// to 1 and silently become channel 1 on a device that never advertised it
// (CHANGELOG 0.1.3); those records are now refused outright.
#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "aoip/aoip_internal.h"

// One mDNS TXT record. value may be NULL for a valueless key ("default");
// value_len is the honest length of value (may carry embedded NULs — the
// parser stops at the first one).
typedef struct {
    const char *key;
    const char *value;
    size_t      value_len;
} aoip_txt_kv_t;

// Parse one _netaudio-chan advertisement. Returns false if anything is
// missing, malformed, or out of range: ints must fit u16, tx_channel_id
// must name one of the advertised channels (1..nchan), fpp bounds are
// 16-bit and nonzero, bit depth is a 16..32 byte-aligned value.
bool mdns_txt_parse_chan(const aoip_txt_kv_t *kv, size_t n, aoip_adv_chan_t *o);

// Parse one _netaudio-bund advertisement. Same rules; the media address
// and port must both be present ("a.0"/"p.0", port nonzero).
bool mdns_txt_parse_bundle(const aoip_txt_kv_t *kv, size_t n, aoip_adv_bundle_t *o);
