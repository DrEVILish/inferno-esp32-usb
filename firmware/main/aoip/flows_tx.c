// SPDX-License-Identifier: GPL-3.0-or-later
//
// TX flows: the flow table (who receives which of our channels) and the
// packetizer. Translated from Inferno device_server/flows_tx.rs.
//
// Control side (ctl task, under aoip_lock): ftx_add_flow & co. mirror
// FlowsTransmitter (flow handle = u32 BE slot index + u16 random cookie, an
// existing destination gets its channels replaced, expired unicast flows are
// reaped when a new flow is added, keepalive timeout 4 s).
//
// Packetizer (core 0, UI_PRIO_TX_PACKETIZER): woken by a one-shot esp_timer
// (ISR dispatch -> task notification) at the next packet boundary of any
// flow. For every active flow, once mc_now_frames() >= next_ts + fpp it
// builds the packet exactly as flows_tx.rs (byte 0 = 2, u32 seconds, u32
// sub-second samples, interleaved big-endian samples), reading frames
// [next_ts, next_ts + fpp) of g_ring_usb_to_net (left-justified int32 -> top
// bits), and sends it with net_udp_send_raw. Packet timestamp = ring frame
// index = media time of the first sample.
//
// The packetizer never takes a lock: the control task publishes a complete
// copy of the table (double buffer + generation counter, acknowledged by the
// packetizer before the other buffer is rewritten).
//
// Keepalives from receivers arrive at our per-slot source port
// (AOIP_TX_PORT_BASE + slot) and are timestamped by a UDP hook in the EMAC RX
// task; Inferno counts any datagram on the flow's connected socket.
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/inet.h"

#include "app_config.h"
#include "aoip/ts_incr.h"
#include "audio/ring.h"
#include "clock/mediaclock.h"
#include "net/net.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"
#include "aoip/wire.h"

static const char *TAG = "aoip_tx";

#define SLOTS AOIP_TX_FLOW_SLOTS
#define DSCP_EF 46u
// Inferno: max_lag_samples = TX latency ("doesn't make sense to send samples
// older than that"). Measured from the moment a packet becomes due.
#define TX_MAX_LAG_FRAMES ((int64_t)UI_NET_LATENCY_FRAMES)

// ---------------------------------------------------------------------------
// Control state
// ---------------------------------------------------------------------------
static ftx_info_t s_info[SLOTS];
static net_udp_dest_t s_dest[SLOTS];
static uint32_t s_instance[SLOTS];
static uint32_t s_resolve_ms[SLOTS];
#define TX_ARP_REFRESH_MS 30000u
static uint32_t s_flow_seq_id;

// Keepalive matching, written by control, read by the hook
static _Atomic uint32_t s_ka_ip[SLOTS];
static _Atomic uint32_t s_ka_port[SLOTS];
static _Atomic uint32_t s_ka_ms[SLOTS];

// ---------------------------------------------------------------------------
// Published table (control -> packetizer)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  active;
    uint8_t  nch;
    uint8_t  bps;
    uint16_t fpp;
    int8_t   ch[UI_TX_MAX_CH_PER_FLOW];
    uint32_t instance;
    net_udp_dest_t dest;
} pub_flow_t;

typedef struct {
    pub_flow_t f[SLOTS];
} pub_table_t;

static pub_table_t s_pub[2];
static _Atomic uint32_t s_pub_gen;
static _Atomic uint32_t s_pub_ack = UINT32_MAX;
static TaskHandle_t s_task;
static esp_timer_handle_t s_timer;
// latency window (telemetry, reset on read): largest send lateness, frames
// after the packet became due (ts + fpp). Must stay well below the AoIP
// latency minus the network transit.
static _Atomic int32_t s_lag_max = INT32_MIN;

int32_t ftx_take_lag_max(void)
{
    return atomic_exchange_explicit(&s_lag_max, INT32_MIN, memory_order_relaxed);
}

static void publish(void)
{
    uint32_t g = atomic_load_explicit(&s_pub_gen, memory_order_relaxed);
    // The packetizer must have switched to generation g (and so stopped
    // reading buffer g+1's previous contents) before we rewrite that buffer.
    for (int k = 0; k < 200 && atomic_load_explicit(&s_pub_ack, memory_order_acquire) != g; k++) {
        if (s_task) xTaskNotifyGive(s_task);
        vTaskDelay(1);
    }
    pub_table_t *t = &s_pub[(g + 1u) & 1u];
    memset(t, 0, sizeof *t);
    for (unsigned i = 0; i < SLOTS; i++) {
        const ftx_info_t *f = &s_info[i];
        pub_flow_t *p = &t->f[i];
        p->active = f->used && !f->expired && s_dest[i].resolved;
        p->nch = f->nch;
        p->bps = f->bps;
        p->fpp = f->fpp;
        memcpy(p->ch, f->ch, sizeof p->ch);
        p->instance = s_instance[i];
        p->dest = s_dest[i];
    }
    atomic_store_explicit(&s_pub_gen, g + 1u, memory_order_release);
    if (s_task) xTaskNotifyGive(s_task);
}

// ---------------------------------------------------------------------------
// Keepalive hook (EMAC RX task, core 0): must not block
// ---------------------------------------------------------------------------
static bool ka_hook(const uint8_t *payload, size_t len, const net_udp_meta_t *m, void *ctx)
{
    unsigned i = (unsigned)(m->dst_port - AOIP_TX_PORT_BASE);
    if (i >= SLOTS) return false;
    if (atomic_load_explicit(&s_ka_ip[i], memory_order_relaxed) == m->src_ip &&
        atomic_load_explicit(&s_ka_port[i], memory_order_relaxed) == m->src_port) {
        atomic_store_explicit(&s_ka_ms[i], (uint32_t)(esp_timer_get_time() / 1000), memory_order_relaxed);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Packetizer
// ---------------------------------------------------------------------------
#if CONFIG_USBINF_TEST_TONE
// One period of 100 Hz at 48 kHz; channel N (1-based) steps N entries per
// frame, so it plays N x 100 Hz, phase-continuous in the media frame index.
#define TONE_LEN (UI_SAMPLE_RATE / 100u)
static int32_t s_tone[TONE_LEN];

static void tone_init(void)
{
    const double amp = 0.1 * 8388607.0;     // -20 dBFS, 24-bit
    for (unsigned i = 0; i < TONE_LEN; i++) {
        double v = amp * sin(2.0 * M_PI * (double)i / (double)TONE_LEN);
        s_tone[i] = (int32_t)lround(v) * 256;
    }
}
#endif

static uint32_t s_dither_state = 0x12345678u;

static inline uint32_t xorshift32(void)
{
    uint32_t x = s_dither_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_dither_state = x;
    return x;
}

static inline int32_t sat_add(int32_t a, int32_t b)
{
    int64_t r = (int64_t)a + b;
    if (r > INT32_MAX) return INT32_MAX;
    if (r < INT32_MIN) return INT32_MIN;
    return (int32_t)r;
}

static uint8_t s_pkt[1536] __attribute__((aligned(4)));

static void build_and_send(const pub_flow_t *f, uint64_t ts, const ts_split_t *split)
{
    perf_begin(PERF_TX_BUILD);
    uint8_t *p = s_pkt;
    const unsigned fpp = f->fpp, nch = f->nch, bps = f->bps;
    const unsigned stride = nch * bps;
    const size_t len = MEDIA_HEADER_LEN + (size_t)fpp * stride;

    p[0] = MEDIA_BYTE0;
    wr32(p + 1, split->sec);
    wr32(p + 5, split->sub);
    uint8_t *o = p + MEDIA_HEADER_LEN;

#if CONFIG_USBINF_TEST_TONE
    uint32_t ph = (uint32_t)(ts % TONE_LEN);
    for (unsigned fi = 0; fi < fpp; fi++) {
        for (unsigned c = 0; c < nch; c++) {
            int ch = f->ch[c];
            int32_t s = ch >= 0 ? s_tone[(ph * (uint32_t)(ch + 1)) % TONE_LEN] : 0;
            if (bps == 3) {
                o[0] = (uint8_t)(s >> 24); o[1] = (uint8_t)(s >> 16); o[2] = (uint8_t)(s >> 8);
            } else if (bps == 2) {
                o[0] = (uint8_t)(s >> 24); o[1] = (uint8_t)(s >> 16);
            } else {
                wr32(o, (uint32_t)s);
            }
            o += bps;
        }
        if (++ph == TONE_LEN) ph = 0;
    }
#else
#if CONFIG_USBINF_TEST_NET_LOOPBACK
    audio_ring_t *r = &g_ring_net_to_usb;
    const uint64_t base = ts - UI_NET_LATENCY_FRAMES;
    const bool valid = true;
#else
    audio_ring_t *r = &g_ring_usb_to_net;
    const uint64_t base = ts;
    const bool valid = ring_valid(r, base, fpp);
#endif
    if (!valid) {
        memset(o, 0, (size_t)fpp * stride);
        CTR_INC(tx_underruns);
        atomic_fetch_add_explicit(&r->underruns, 1u, memory_order_relaxed);
    } else if (bps == 3) {
        for (unsigned fi = 0; fi < fpp; fi++) {
            const int32_t *fr = ring_frame(r, base + fi);
            for (unsigned c = 0; c < nch; c++) {
                int ch = f->ch[c];
                int32_t s = ch >= 0 ? fr[ch] : 0;
                o[0] = (uint8_t)(s >> 24); o[1] = (uint8_t)(s >> 16); o[2] = (uint8_t)(s >> 8);
                o += 3;
            }
        }
    } else if (bps == 2) {
        // Source is 24-bit: TPDF dither to 16 bits as samples_utils.rs
        // (sample + rand_u16 - rand_u16 + 2^15, saturating).
        for (unsigned fi = 0; fi < fpp; fi++) {
            const int32_t *fr = ring_frame(r, base + fi);
            for (unsigned c = 0; c < nch; c++) {
                int ch = f->ch[c];
                int32_t s = 0;
                if (ch >= 0) {
                    uint32_t rnd = xorshift32();
                    int32_t d = (int32_t)(rnd & 0xffffu) - (int32_t)(rnd >> 16) + 32768;
                    s = sat_add(fr[ch], d);
                }
                o[0] = (uint8_t)(s >> 24); o[1] = (uint8_t)(s >> 16);
                o += 2;
            }
        }
    } else {
        for (unsigned fi = 0; fi < fpp; fi++) {
            const int32_t *fr = ring_frame(r, base + fi);
            for (unsigned c = 0; c < nch; c++) {
                int ch = f->ch[c];
                wr32(o, (uint32_t)(ch >= 0 ? fr[ch] : 0));
                o += 4;
            }
        }
    }
#endif

    perf_begin(PERF_TX_SEND);
    esp_err_t snd = net_udp_send_raw(&f->dest, p, len);
    perf_end(PERF_TX_SEND);
    if (snd == ESP_OK) {
        CTR_INC(tx_packets);
    } else {
        CTR_INC(tx_send_fail);
    }
    perf_end(PERF_TX_BUILD);
}

static void IRAM_ATTR timer_cb(void *arg)
{
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &hp);
    if (hp == pdTRUE) portYIELD_FROM_ISR();
}

typedef struct {
    uint32_t instance;
    bool     boot;
    uint64_t next_ts;
    ts_split_t split;       // (sec, sub) of next_ts, kept incrementally
} rt_flow_t;

static inline void bootstrap(rt_flow_t *rt, uint64_t now, unsigned fpp)
{
    // flows_tx.rs bootstrap_next_ts: next multiple of fpp after now
    rt->next_ts = now + fpp - now % fpp;
    // the divisions happen here only, once per (re)anchor
    ts_split_load(&rt->split, rt->next_ts);
}

static void packetizer_task(void *arg)
{
    static pub_table_t cur;
    static rt_flow_t rt[SLOTS];
    uint32_t my_gen = UINT32_MAX;
    uint32_t epoch = mc_epoch();
    memset(&cur, 0, sizeof cur);

    for (;;) {
        uint32_t g = atomic_load_explicit(&s_pub_gen, memory_order_acquire);
        if (g != my_gen) {
            memcpy(&cur, &s_pub[g & 1u], sizeof cur);
            my_gen = g;
            atomic_store_explicit(&s_pub_ack, g, memory_order_release);
            for (unsigned i = 0; i < SLOTS; i++) {
                if (cur.f[i].instance != rt[i].instance) {
                    rt[i].instance = cur.f[i].instance;
                    rt[i].boot = true;
                }
            }
        }
        // time and epoch read as a pair: a step between two separate reads
        // would bootstrap the flows on the old timescale under the new epoch
        uint32_t e;
        const uint64_t now = mc_ns_to_frames(mc_now_ns_epoch(&e));
        if (e != epoch) {
            // the media clock was stepped: re-anchor every flow
            epoch = e;
            for (unsigned i = 0; i < SLOTS; i++) rt[i].boot = true;
        }

        int64_t wake = INT64_MAX;
        for (unsigned i = 0; i < SLOTS; i++) {
            const pub_flow_t *f = &cur.f[i];
            if (!f->active || f->fpp == 0) continue;
            rt_flow_t *r = &rt[i];
            const int64_t fpp = f->fpp;
            if (r->boot) {
                bootstrap(r, now, f->fpp);
                r->boot = false;
            }
            int64_t lag = (int64_t)(now - (r->next_ts + (uint64_t)fpp));
            if (lag > TX_MAX_LAG_FRAMES || lag < -(int64_t)TX_DISCONTINUITY_FRAMES - fpp) {
                // "tx lag detected, or media clock jumped": re-anchor
                bootstrap(r, now, f->fpp);
                CTR_INC(tx_lag_resets);
                lag = (int64_t)(now - (r->next_ts + (uint64_t)fpp));
            }
            if (lag >= 0 && lag > atomic_load_explicit(&s_lag_max, memory_order_relaxed)) {
                atomic_store_explicit(&s_lag_max, (int32_t)lag, memory_order_relaxed);
            }
            while (lag >= 0) {
                build_and_send(f, r->next_ts, &r->split);
                r->next_ts += (uint64_t)fpp;
                ts_split_advance(&r->split, f->fpp);
                lag -= fpp;
            }
            if (-lag < wake) wake = -lag;
        }

        if (wake == INT64_MAX) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        // frames -> microseconds, rounded up; esp_timer runs on the system
        // timer, not the PTP clock, so we re-read the media clock each wake.
        uint64_t us = ((uint64_t)wake * 1000000u + UI_SAMPLE_RATE - 1u) / UI_SAMPLE_RATE;
        if (us < 10u) us = 10u;
        esp_timer_stop(s_timer);
        esp_timer_start_once(s_timer, us);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

// ---------------------------------------------------------------------------
// Control API
// ---------------------------------------------------------------------------
esp_err_t ftx_init(void)
{
#if CONFIG_USBINF_TEST_TONE
    tone_init();
#endif
    memset(s_info, 0, sizeof s_info);
    s_flow_seq_id = 0;
    esp_timer_create_args_t ta = {
        .callback = timer_cb,
        .dispatch_method = ESP_TIMER_ISR,
        .name = "aoip_tx",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&ta, &s_timer);
    if (err != ESP_OK) return err;
    BaseType_t ok = xTaskCreatePinnedToCore(packetizer_task, "aoip_tx", 4096, NULL,
                                            UI_PRIO_TX_PACKETIZER, &s_task, UI_CORE_NET);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    err = net_udp_hook_register(AOIP_TX_PORT_BASE, AOIP_TX_PORT_BASE + SLOTS - 1u, ka_hook, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "keepalive hook: %s", esp_err_to_name(err));
    return ESP_OK;
}

static void set_keepalive_match(unsigned i, uint32_t ip, uint16_t port)
{
    atomic_store(&s_ka_ip[i], ip);
    atomic_store(&s_ka_port[i], port);
    atomic_store(&s_ka_ms[i], aoip_now_ms());
}

static void remove_internal(unsigned i)
{
    s_info[i].used = false;
    s_dest[i].resolved = false;
    set_keepalive_match(i, 0, 0);
    publish();
}

static void scan_expired(void)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        if (s_info[i].used && s_info[i].expired && !s_info[i].multicast) {
            ESP_LOGI(TAG, "removing expired flow %u", i);
            remove_internal(i);
        }
    }
}

static int find_destination(uint32_t ip, uint16_t port)
{
    for (unsigned i = 0; i < SLOTS; i++) {
        if (s_info[i].used && s_info[i].dst_ip == ip && s_info[i].dst_port == port) return (int)i;
    }
    return -1;
}

bool ftx_destination_exists(uint32_t ip, uint16_t port)
{
    return find_destination(ip, port) >= 0;
}

void ftx_random_multicast_destination(uint32_t *ip, uint16_t *port)
{
    for (;;) {
        uint32_t r = esp_random();
        // 239.255.x.y
        uint32_t a = htonl(0xEFFF0000u | (r & 0xffffu));
        if (!ftx_destination_exists(a, AOIP_PORT_MEDIA_MCAST)) {
            *ip = a;
            *port = AOIP_PORT_MEDIA_MCAST;
            return;
        }
    }
}

static void make_handle(unsigned index, uint16_t cookie, uint8_t h[6])
{
    wr32(h, index);
    wr16(h + 4, cookie);
}

static int handle_to_index(const uint8_t h[6])
{
    uint32_t index = rd32(h);
    uint16_t cookie = rd16(h + 4);
    if (index >= SLOTS || !s_info[index].used || s_info[index].cookie != cookie) return -1;
    return (int)index;
}

esp_err_t ftx_add_flow(const ftx_info_t *info, int requested_index, bool is_multicast,
                       unsigned *index_out, uint8_t handle_out[6])
{
    int idx = find_destination(info->dst_ip, info->dst_port);
    if (idx >= 0) {
        // "got add flow request for already existing flow, setting channels instead"
        ftx_info_t *f = &s_info[idx];
        ESP_LOGW(TAG, "add flow for existing destination: updating flow %d", idx);
        uint16_t cookie = f->cookie;
        bool mc = f->multicast, expired = f->expired;
        *f = *info;
        f->used = true;
        f->cookie = cookie;
        f->multicast = mc;
        f->expired = expired && mc;     // a unicast request resuscitates the flow
        s_instance[idx]++;              // fpp/encoding may have changed: re-anchor
        atomic_store(&s_ka_ms[idx], aoip_now_ms());
        publish();
        *index_out = (unsigned)idx;
        make_handle((unsigned)idx, cookie, handle_out);
        return ESP_OK;
    }

    scan_expired();
    if (requested_index >= 0) {
        if (requested_index >= (int)SLOTS || s_info[requested_index].used) {
            ESP_LOGW(TAG, "requested flow index %d busy", requested_index);
            return ESP_ERR_INVALID_STATE;
        }
        idx = requested_index;
    } else {
        unsigned counter = 0;
        for (;;) {
            unsigned i = s_flow_seq_id++ % SLOTS;
            if (!s_info[i].used) {
                idx = (int)i;
                break;
            }
            if (++counter > SLOTS) {
                ESP_LOGW(TAG, "ran out of flows (%u)", SLOTS);
                return ESP_ERR_NO_MEM;
            }
        }
    }

    ftx_info_t *f = &s_info[idx];
    *f = *info;
    f->used = true;
    f->multicast = is_multicast;
    // multicast flows start "expired" for the address-conflict grace period
    f->expired = is_multicast;
    f->cookie = (uint16_t)esp_random();
    s_dest[idx] = (net_udp_dest_t){
        .dst_ip = info->dst_ip,
        .dst_port = info->dst_port,
        .src_port = (uint16_t)(AOIP_TX_PORT_BASE + (unsigned)idx),
        .dscp = DSCP_EF,
        .resolved = false,
    };
    net_udp_dest_resolve(&s_dest[idx]);
    s_resolve_ms[idx] = aoip_now_ms();
    s_instance[idx]++;
    set_keepalive_match((unsigned)idx, is_multicast ? 0 : info->dst_ip, is_multicast ? 0 : info->dst_port);
    publish();
    *index_out = (unsigned)idx;
    make_handle((unsigned)idx, f->cookie, handle_out);
    return ESP_OK;
}

esp_err_t ftx_remove_flow(const uint8_t handle[6])
{
    int i = handle_to_index(handle);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    remove_internal((unsigned)i);
    return ESP_OK;
}

esp_err_t ftx_remove_multicast_flow(unsigned index)
{
    if (index >= SLOTS || !s_info[index].used) return ESP_ERR_NOT_FOUND;
    if (!s_info[index].multicast) {
        ESP_LOGW(TAG, "refusing to remove unicast flow %u without its handle", index);
        return ESP_ERR_NOT_ALLOWED;
    }
    remove_internal(index);
    return ESP_OK;
}

esp_err_t ftx_set_channels(const uint8_t handle[6], const int8_t *ch, unsigned n)
{
    int i = handle_to_index(handle);
    if (i < 0) return ESP_ERR_NOT_FOUND;
    ftx_info_t *f = &s_info[i];
    if (n > UI_TX_MAX_CH_PER_FLOW || MEDIA_HEADER_LEN + (size_t)f->fpp * n * f->bps > 1472u) {
        return ESP_ERR_INVALID_SIZE;
    }
    f->nch = (uint8_t)n;
    memset(f->ch, -1, sizeof f->ch);
    memcpy(f->ch, ch, n);
    atomic_store(&s_ka_ms[i], aoip_now_ms());
    if (f->expired) {
        ESP_LOGI(TAG, "resuscitating expired flow %d", i);
        f->expired = false;
        s_instance[i]++;
    }
    publish();
    return ESP_OK;
}

void ftx_activate_multicast_flow(unsigned index)
{
    if (index >= SLOTS || !s_info[index].used) return;
    s_info[index].expired = false;
    s_instance[index]++;
    publish();
}

const ftx_info_t *ftx_get(unsigned index)
{
    return (index < SLOTS && s_info[index].used) ? &s_info[index] : NULL;
}

unsigned ftx_active_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < SLOTS; i++) n += s_info[i].used && !s_info[i].expired;
    return n;
}

void ftx_tick(void)
{
    const uint32_t now = aoip_now_ms();
    bool changed = false;
    for (unsigned i = 0; i < SLOTS; i++) {
        ftx_info_t *f = &s_info[i];
        if (!f->used) continue;
        if (!f->multicast && !f->expired &&
            now - atomic_load(&s_ka_ms[i]) > TX_KEEPALIVE_TIMEOUT_S * 1000u) {
            ESP_LOGI(TAG, "flow %u to " IPSTR ":%u expired (no keepalives)", i,
                     IP2STR((esp_ip4_addr_t *)&f->dst_ip), f->dst_port);
            f->expired = true;
            changed = true;
        }
        if (!s_dest[i].resolved) {
            if (net_udp_dest_resolve(&s_dest[i]) == ESP_OK && s_dest[i].resolved) {
                s_resolve_ms[i] = now;
                changed = true;
            }
        } else if (now - s_resolve_ms[i] > TX_ARP_REFRESH_MS) {
            // ARP entries age out and peers can change address: re-resolve
            // into a copy and keep sending to the old MAC meanwhile.
            net_udp_dest_t d = s_dest[i];
            d.resolved = false;
            if (net_udp_dest_resolve(&d) == ESP_OK && d.resolved) {
                s_resolve_ms[i] = now;
                if (memcmp(d.dst_mac, s_dest[i].dst_mac, 6) != 0) {
                    s_dest[i] = d;
                    changed = true;
                }
            }
        }
        // A source-address change (DHCP renew, lease expiry) must reach the
        // packetizer through a publish: the template in the published copy
        // still carries the old source otherwise.
        if (s_dest[i].resolved && net_udp_dest_seal(&s_dest[i])) {
            changed = true;
        }
    }
    if (changed) publish();
}
