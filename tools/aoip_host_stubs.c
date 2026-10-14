// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-test stubs for tools/arc_info_host_test.c: fake device state,
// controllable clock, and a sendto capture. Links against the REAL
// firmware/main/aoip/arc.c, wire.c and info_mcast.c.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "app_config.h"
#include "audio/ring.h"
#include "aoip/aoip_internal.h"

// ---------------------------------------------------------------------------
// Device state
// ---------------------------------------------------------------------------
aoip_dev_t g_aoip;
aoip_counters_t g_aoip_ctr;
audio_ring_t g_ring_net_to_usb;
audio_ring_t g_ring_usb_to_net;

// Controllable clock (mc_*): leader identity + frequency offset.
uint8_t host_leader_id[8];
int32_t host_freq_ppb;

void mc_leader_id(uint8_t id[8]) { memcpy(id, host_leader_id, 8); }
int32_t mc_freq_ppb(void) { return host_freq_ppb; }
uint64_t mc_now_frames(void) { return 0; }

// ---------------------------------------------------------------------------
// AoIP modules not under test: inert.
// ---------------------------------------------------------------------------
void aoip_factory_ch_name(char out[4], unsigned index)
{
    snprintf(out, 4, "%02u", index + 1u);
}

void sub_get_info(unsigned local_ch, sub_info_t *out)
{
    (void)local_ch;
    memset(out, 0, sizeof *out);
}

void sub_subscribe(unsigned local_ch, const char *txch, const char *host)
{
    (void)local_ch;
    (void)txch;
    (void)host;
}

void sub_unsubscribe(unsigned local_ch) { (void)local_ch; }

const ftx_info_t *ftx_get(unsigned index)
{
    (void)index;
    return 0;
}

bool txm_add_flow(unsigned flow_index, const int8_t *ch, unsigned n)
{
    (void)flow_index;
    (void)ch;
    (void)n;
    return false;
}

esp_err_t txm_remove_flow(unsigned flow_index)
{
    (void)flow_index;
    return ESP_FAIL;
}

bool frx_get_info(unsigned slot, frx_info_t *out)
{
    (void)slot;
    (void)out;
    return false;
}

int32_t frx_take_max_latency(unsigned slot)
{
    (void)slot;
    return 0;
}

void frx_take_peaks(uint32_t peaks[UI_CHANNELS])
{
    memset(peaks, 0, sizeof(uint32_t) * UI_CHANNELS);
}

void aoip_mdns_remove_tx_channel_name(unsigned index, const char *name)
{
    (void)index;
    (void)name;
}

void aoip_mdns_refresh_tx_channel(unsigned index) { (void)index; }
void aoip_mdns_request_republish(void) {}
void settings_mark_dirty(uint32_t bits) { (void)bits; }

// ---------------------------------------------------------------------------
// sendto capture (info multicasts + heartbeat).
// ---------------------------------------------------------------------------
#define HOST_CAP_N 16

static uint8_t cap_buf[HOST_CAP_N][2048];
static size_t cap_len[HOST_CAP_N];
static uint8_t cap_op[HOST_CAP_N][8];
static unsigned cap_n;

ssize_t host_sendto_capture(int s, const void *buf, size_t len, int flags,
                            const struct sockaddr *dst, socklen_t slen)
{
    (void)s;
    (void)flags;
    (void)dst;
    (void)slen;
    if (cap_n < HOST_CAP_N && len <= sizeof cap_buf[0]) {
        memcpy(cap_buf[cap_n], buf, len);
        cap_len[cap_n] = len;
        // multicast opcode lives at bytes 24..31
        if (len >= 32) memcpy(cap_op[cap_n], (const uint8_t *)buf + 24, 8);
        cap_n++;
    }
    return (ssize_t)len;
}

void host_cap_reset(void) { cap_n = 0; }
unsigned host_cap_count(void) { return cap_n; }
const uint8_t *host_cap_pkt(unsigned i, size_t *len_out)
{
    *len_out = cap_len[i];
    return cap_buf[i];
}
const uint8_t *host_cap_op(unsigned i) { return cap_op[i]; }
