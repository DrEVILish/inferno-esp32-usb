// SPDX-License-Identifier: GPL-3.0-or-later
//
// RX flows: decoding received audio straight into g_ring_net_to_usb, and the
// flow keepalives. Translated from Inferno device_server/flows_rx.rs.
//
// Audio packets never reach lwIP: a UDP hook (net/, EMAC RX task, core 0)
// claims our RX port range (AOIP_RX_PORT_BASE + slot) and the multicast
// media port, and decodes each packet in place:
//   frame = seconds * fs + subsec   (the packet's media timestamp)
//   ring slot = frame + shift, shift = flow latency - UI_NET_LATENCY_FRAMES
// so a flow whose transmitter demands more than our latency still plays at
// its own latency (Inferno's per-channel timestamp_shift). The USB IN side
// reads (and clears) at now - UI_NET_LATENCY_FRAMES; packets whose first
// frame is already due are dropped as late, packets beyond the ring window
// as early. 16/24/32-bit big-endian samples become left-justified int32.
//
// The hook is lock-free: per-slot configuration is changed only while the
// slot is inactive (plus a 2 ms quiet period), the channel map is double
// buffered and switched atomically.
//
// Keepalives (13 37) go every 250 ms from the flow's own port to the source
// address of its audio packets, as flows_rx.rs does; without them a
// transmitter drops the flow after ~4 s. They are sent from the control task
// through an lwIP socket bound to the RX port.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "audio/ring.h"
#include "clock/mediaclock.h"
#include "net/net.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"
#include "aoip/wire.h"

static const char *TAG = "aoip_rx";

#define SLOTS AOIP_RX_FLOW_SLOTS
// Never write closer than this to the frames the reader will reach a ring lap later.
#define RX_EARLY_GUARD 64u
// Largest extra shift (flow latency above ours) that still fits the ring.
#define RX_MAX_SHIFT ((int32_t)(RING_FRAMES / 2u))

typedef struct {
    uint16_t src_off;       // byte offset of the channel within a frame
    uint8_t  dst;           // local RX channel (ring column)
} rx_pair_t;

typedef struct {
    uint8_t   n;
    rx_pair_t p[UI_CHANNELS];
} rx_map_t;

typedef struct {
    _Atomic uint8_t  active;
    uint8_t   bps, nch;
    uint16_t  stride;
    int32_t   shift;
    uint32_t  mcast_ip;     // 0 = unicast
    rx_map_t  map[2];
    _Atomic uint8_t  map_idx;
    // written by the hook
    _Atomic uint32_t last_ms;
    _Atomic uint32_t src_ip;
    _Atomic uint32_t src_port;
    _Atomic int32_t  max_lat;
    _Atomic int32_t  min_margin;    // latency window: frames a packet beat its playout deadline by
    uint64_t  expect_ts;
    bool      have_ts;
} rx_slot_t;

static rx_slot_t s_slot[SLOTS];
static uint32_t s_peak[UI_CHANNELS];

// control side (aoip_lock)
static frx_info_t s_ctl[SLOTS];
static int s_ka_sock[SLOTS];
static bool s_send_ka[SLOTS];
static uint32_t s_map_switch_ms[SLOTS];

uint16_t frx_port(unsigned slot) { return (uint16_t)(AOIP_RX_PORT_BASE + slot); }

// ---------------------------------------------------------------------------
// Hook (EMAC RX task, core 0): fast, no blocking, no logging
// ---------------------------------------------------------------------------
static inline uint32_t abs32(int32_t s) { return s < 0 ? (uint32_t)(-(int64_t)s) : (uint32_t)s; }

static bool rx_hook(const uint8_t *pl, size_t len, const net_udp_meta_t *meta, void *ctx)
{
    unsigned slot;
    if (meta->dst_port == AOIP_PORT_MEDIA_MCAST) {
        for (slot = 0; slot < SLOTS; slot++) {
            if (s_slot[slot].mcast_ip == meta->dst_ip &&
                atomic_load_explicit(&s_slot[slot].active, memory_order_acquire)) break;
        }
        if (slot == SLOTS) return false;
    } else {
        slot = (unsigned)(meta->dst_port - AOIP_RX_PORT_BASE);
        if (slot >= SLOTS) return false;
    }
    rx_slot_t *s = &s_slot[slot];
    if (!atomic_load_explicit(&s->active, memory_order_acquire)) return true;

    const unsigned stride = s->stride, bps = s->bps;
    if (len <= MEDIA_HEADER_LEN || stride == 0) {
        CTR_INC(rx_wrong_len);
        return true;
    }
    const size_t alen = len - MEDIA_HEADER_LEN;
    const unsigned n = (unsigned)(alen / stride);
    if (n == 0 || (size_t)n * stride != alen) {
        CTR_INC(rx_wrong_len);
        return true;
    }

    const uint64_t ts = (uint64_t)rd32(pl + 1) * UI_SAMPLE_RATE + rd32(pl + 5);
    const uint64_t now = mc_now_frames();
    CTR_INC(rx_packets);
    atomic_store_explicit(&s->last_ms, (uint32_t)(esp_timer_get_time() / 1000), memory_order_relaxed);
    atomic_store_explicit(&s->src_ip, meta->src_ip, memory_order_relaxed);
    atomic_store_explicit(&s->src_port, meta->src_port, memory_order_relaxed);

    int64_t lat = (int64_t)(now - ts);
    if (lat > INT32_MAX) lat = INT32_MAX;
    if (lat > atomic_load_explicit(&s->max_lat, memory_order_relaxed)) {
        atomic_store_explicit(&s->max_lat, (int32_t)lat, memory_order_relaxed);
    }
    if (s->have_ts && ts != s->expect_ts && (int64_t)(ts - s->expect_ts) > 0) CTR_INC(rx_seq_gaps);
    s->expect_ts = ts + n;
    s->have_ts = true;

    const uint64_t w = ts + (uint64_t)(int64_t)s->shift;
    const uint64_t read_pos = now - UI_NET_LATENCY_FRAMES;
    const int64_t ahead = (int64_t)(w - read_pos);
    // arrival margin = packet ts + flow latency - arrival time (frames): how
    // much earlier than its playout deadline the packet arrived (< 0: late)
    {
        const int32_t mg = ahead < INT32_MIN ? INT32_MIN : (ahead > INT32_MAX ? INT32_MAX : (int32_t)ahead);
        if (mg < atomic_load_explicit(&s->min_margin, memory_order_relaxed)) {
            atomic_store_explicit(&s->min_margin, mg, memory_order_relaxed);
        }
    }
    if (ahead < 0) {
        CTR_INC(rx_late);
        atomic_fetch_add_explicit(&g_ring_net_to_usb.late, 1u, memory_order_relaxed);
        return true;
    }
    if (ahead + n > (int64_t)(RING_FRAMES - RX_EARLY_GUARD)) {
        CTR_INC(rx_early);
        return true;
    }

    const rx_map_t *m = &s->map[atomic_load_explicit(&s->map_idx, memory_order_acquire) & 1u];
    const unsigned np = m->n;
    if (np == 0) return true;
    uint32_t pk[UI_CHANNELS] = { 0 };
    audio_ring_t *r = &g_ring_net_to_usb;
    const uint8_t *src = pl + MEDIA_HEADER_LEN;

    if (bps == 3) {
        for (unsigned fi = 0; fi < n; fi++, src += stride) {
            int32_t *fr = ring_frame(r, w + fi);
            for (unsigned k = 0; k < np; k++) {
                const uint8_t *b = src + m->p[k].src_off;
                int32_t v = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8));
                fr[m->p[k].dst] = v;
                uint32_t a = abs32(v);
                if (a > pk[k]) pk[k] = a;
            }
        }
    } else if (bps == 2) {
        for (unsigned fi = 0; fi < n; fi++, src += stride) {
            int32_t *fr = ring_frame(r, w + fi);
            for (unsigned k = 0; k < np; k++) {
                const uint8_t *b = src + m->p[k].src_off;
                int32_t v = (int32_t)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16));
                fr[m->p[k].dst] = v;
                uint32_t a = abs32(v);
                if (a > pk[k]) pk[k] = a;
            }
        }
    } else {
        for (unsigned fi = 0; fi < n; fi++, src += stride) {
            int32_t *fr = ring_frame(r, w + fi);
            for (unsigned k = 0; k < np; k++) {
                int32_t v = (int32_t)rd32(src + m->p[k].src_off);
                // ring holds 24-bit left-justified samples
                v &= (int32_t)0xFFFFFF00;
                fr[m->p[k].dst] = v;
                uint32_t a = abs32(v);
                if (a > pk[k]) pk[k] = a;
            }
        }
    }
    for (unsigned k = 0; k < np; k++) {
        unsigned d = m->p[k].dst;
        if (pk[k] > s_peak[d]) s_peak[d] = pk[k];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Control side
// ---------------------------------------------------------------------------
esp_err_t frx_init(void)
{
    memset(s_slot, 0, sizeof s_slot);
    memset(s_ctl, 0, sizeof s_ctl);
    for (unsigned i = 0; i < SLOTS; i++) s_ka_sock[i] = -1;
    esp_err_t err = net_udp_hook_register(AOIP_RX_PORT_BASE, AOIP_RX_PORT_BASE + SLOTS - 1u, rx_hook, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "unicast hook: %s", esp_err_to_name(err));
    err = net_udp_hook_register(AOIP_PORT_MEDIA_MCAST, AOIP_PORT_MEDIA_MCAST, rx_hook, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "multicast hook: %s", esp_err_to_name(err));
    return ESP_OK;
}

static int open_ka_socket(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static void quiesce(void)
{
    // let a hook call that saw the old state finish (it runs for microseconds)
    vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);
}

esp_err_t frx_add_flow(unsigned slot, uint32_t mcast_ip, unsigned bps, unsigned nch,
                       uint32_t latency_frames, bool send_keepalives)
{
    if (slot >= SLOTS || nch == 0 || nch > AOIP_RX_MAX_CH_PER_FLOW || bps < 2 || bps > 4) {
        return ESP_ERR_INVALID_ARG;
    }
    // Phase 1 (locked, brief): deactivate and detach the old resources.
    // (Socket setup and the 2 ms quiesce below touch no shared state and
    // run unlocked; the hook bails on inactive slots, so no new hook call
    // can observe the half-rebuilt state.)
    bool was_active;
    int old_sock;
    aoip_lock();
    rx_slot_t *s = &s_slot[slot];
    was_active = atomic_load(&s->active);
    if (was_active) atomic_store_explicit(&s->active, 0, memory_order_release);
    old_sock = s_ka_sock[slot];
    s_ka_sock[slot] = -1;
    s_send_ka[slot] = false;
    memset(&s_ctl[slot], 0, sizeof s_ctl[slot]);
    aoip_unlock();

    if (was_active) quiesce();
    if (old_sock >= 0) close(old_sock);

    int new_sock = -1;
    if (send_keepalives) {
        new_sock = open_ka_socket(frx_port(slot));
        if (new_sock < 0) {
            ESP_LOGE(TAG, "keepalive socket for port %u failed", frx_port(slot));
            return ESP_FAIL;
        }
    }

    // Locked install (brief): the new state. No blocking calls.
    aoip_lock();
    s = &s_slot[slot];
    int32_t shift = (int32_t)latency_frames - (int32_t)UI_NET_LATENCY_FRAMES;
    if (shift < 0) shift = 0;
    if (shift > RX_MAX_SHIFT) shift = RX_MAX_SHIFT;
    s->bps = (uint8_t)bps;
    s->nch = (uint8_t)nch;
    s->stride = (uint16_t)(bps * nch);
    s->shift = shift;
    s->mcast_ip = mcast_ip;
    memset(s->map, 0, sizeof s->map);
    atomic_store(&s->map_idx, 0);
    atomic_store(&s->last_ms, 0);
    atomic_store(&s->src_ip, 0);
    atomic_store(&s->src_port, 0);
    atomic_store(&s->max_lat, 0);
    atomic_store(&s->min_margin, INT32_MAX);
    s->have_ts = false;
    if (new_sock >= 0) {
        s_ka_sock[slot] = new_sock;
        s_send_ka[slot] = true;
    }

    frx_info_t *c = &s_ctl[slot];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->rx_port = mcast_ip ? AOIP_PORT_MEDIA_MCAST : frx_port(slot);
    c->nch = (uint8_t)nch;
    c->latency_frames = latency_frames;
    atomic_store_explicit(&s->active, 1, memory_order_release);
    aoip_unlock();

    // IGMP join round-trips through the tcpip thread: after the slot is
    // live, so it never extends the lock hold.
    if (mcast_ip) net_mcast_join(mcast_ip);
    return ESP_OK;
}

void frx_remove_flow(unsigned slot)
{
    if (slot >= SLOTS) return;
    // Phase 1 (locked, brief): deactivate and detach.
    bool was_active;
    int old_sock;
    uint32_t group;
    bool still_used;
    aoip_lock();
    rx_slot_t *s = &s_slot[slot];
    was_active = atomic_load(&s->active);
    if (was_active) atomic_store_explicit(&s->active, 0, memory_order_release);
    old_sock = s_ka_sock[slot];
    s_ka_sock[slot] = -1;
    s_send_ka[slot] = false;
    group = s->mcast_ip;
    s->mcast_ip = 0;
    memset(&s_ctl[slot], 0, sizeof s_ctl[slot]);
    still_used = false;
    if (group) {
        for (unsigned i = 0; i < SLOTS; i++) {
            still_used |= (s_ctl[i].used && i != slot && s_slot[i].mcast_ip == group);
        }
    }
    aoip_unlock();

    // Phase 2 (unlocked): quiesce, socket close, multicast leave.
    if (was_active) quiesce();
    if (old_sock >= 0) close(old_sock);
    if (group && !still_used) net_mcast_leave(group);
}

static void rebuild_map(unsigned slot)
{
    rx_slot_t *s = &s_slot[slot];
    const frx_info_t *c = &s_ctl[slot];
    // the hook may still read the buffer we are about to rewrite if the
    // previous switch was very recent
    uint32_t now = aoip_now_ms();
    if (now - s_map_switch_ms[slot] < 3u) quiesce();
    unsigned cur = atomic_load_explicit(&s->map_idx, memory_order_relaxed) & 1u;
    rx_map_t *m = &s->map[cur ^ 1u];
    memset(m, 0, sizeof *m);
    for (unsigned cif = 0; cif < c->nch; cif++) {
        for (unsigned ch = 0; ch < UI_CHANNELS; ch++) {
            if ((c->channels_map[cif] & (1u << ch)) && m->n < UI_CHANNELS) {
                m->p[m->n].src_off = (uint16_t)(cif * s->bps);
                m->p[m->n].dst = (uint8_t)ch;
                m->n++;
            }
        }
    }
    atomic_store_explicit(&s->map_idx, (uint8_t)(cur ^ 1u), memory_order_release);
    s_map_switch_ms[slot] = aoip_now_ms();
}

void frx_connect_channel(unsigned slot, unsigned ch_in_flow, unsigned local_ch)
{
    if (slot >= SLOTS || local_ch >= UI_CHANNELS) return;
    aoip_lock();
    bool ok = s_ctl[slot].used && ch_in_flow < s_ctl[slot].nch;
    if (ok) s_ctl[slot].channels_map[ch_in_flow] |= 1u << local_ch;
    aoip_unlock();
    // rebuild_map() can quiesce (2 ms) when the previous switch was very
    // recent: run it without holding aoip_lock. Only the subscriber task
    // mutates channels_map, and the hook reads the new table through the
    // map_idx acquire/release handoff.
    if (ok) rebuild_map(slot);
}

void frx_disconnect_channel(unsigned slot, unsigned ch_in_flow, unsigned local_ch)
{
    if (slot >= SLOTS || local_ch >= UI_CHANNELS) return;
    aoip_lock();
    bool ok = s_ctl[slot].used && ch_in_flow < s_ctl[slot].nch;
    if (ok) s_ctl[slot].channels_map[ch_in_flow] &= ~(1u << local_ch);
    aoip_unlock();
    if (ok) rebuild_map(slot);
}

uint32_t frx_last_packet_ms(unsigned slot)
{
    return slot < SLOTS ? atomic_load_explicit(&s_slot[slot].last_ms, memory_order_relaxed) : 0;
}

bool frx_get_info(unsigned slot, frx_info_t *out)
{
    if (slot >= SLOTS) return false;
    aoip_lock();
    bool used = s_ctl[slot].used;
    if (used) *out = s_ctl[slot];
    aoip_unlock();
    return used;
}

int32_t frx_take_max_latency(unsigned slot)
{
    if (slot >= SLOTS || !s_ctl[slot].used) return 0;
    return atomic_exchange_explicit(&s_slot[slot].max_lat, 0, memory_order_relaxed);
}

int32_t frx_take_min_margin(unsigned slot)
{
    if (slot >= SLOTS || !s_ctl[slot].used) return INT32_MAX;
    return atomic_exchange_explicit(&s_slot[slot].min_margin, INT32_MAX, memory_order_relaxed);
}

void frx_take_peaks(uint32_t peaks[UI_CHANNELS])
{
    for (unsigned i = 0; i < UI_CHANNELS; i++) {
        peaks[i] = s_peak[i];
        s_peak[i] = 0;      // racy with the hook by design: a lost peak only dims one meter update
    }
}

unsigned frx_active_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SLOTS; i++) n += s_ctl[i].used;
    return n;
}

void frx_keepalive_tick(void)
{
    static const uint8_t ka[2] = MEDIA_KEEPALIVE;
    for (unsigned i = 0; i < SLOTS; i++) {
        if (!s_ctl[i].used || !s_send_ka[i] || s_ka_sock[i] < 0) continue;
        uint32_t ip = atomic_load_explicit(&s_slot[i].src_ip, memory_order_relaxed);
        uint32_t port = atomic_load_explicit(&s_slot[i].src_port, memory_order_relaxed);
        if (!ip || !port) continue;     // no packet yet: source unknown (as Inferno)
        struct sockaddr_in a = {
            .sin_family = AF_INET,
            .sin_port = htons((uint16_t)port),
            .sin_addr.s_addr = ip,
        };
        if (sendto(s_ka_sock[i], ka, sizeof ka, 0, (struct sockaddr *)&a, sizeof a) == (int)sizeof ka) {
            CTR_INC(rx_ka_sent);
        } else {
            CTR_INC(rx_ka_fail);
        }
    }
}
