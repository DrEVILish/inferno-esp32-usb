// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ethernet bring-up (internal EMAC + IP101 over RMII), the receive fast path
// that runs ahead of lwIP, raw UDP transmit, and IGMP membership upkeep.
// See docs/11-net-clock.md for the reasoning behind each register setting.
#include "net/net.h"
#include "net/net_priv.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_cpu.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_rom_sys.h"
#include "lwip/etharp.h"
#include "lwip/igmp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "soc/emac_dma_struct.h"
#include "soc/emac_ptp_struct.h"

#include "app_config.h"
#include "sys/perf.h"

static const char *TAG = "net";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define ETH_HDR_LEN         14u
#define IP_HDR_LEN          20u
#define UDP_HDR_LEN         8u
#define RAW_HDR_LEN         (ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN)    // 42
#define ETHERTYPE_IPV4      0x0800u
#define IPPROTO_UDP_NUM     17u
#define RAW_MAX_PAYLOAD     1472u

#define MAX_HOOKS           16
#define MAX_GROUPS          32

#define EMAC_RX_STACK       6144u       // hooks (audio decode) run on this stack

// TDES0 "Transmit Timestamp Enable" (TTSE), bit 25 of the first descriptor
// word in the Synopsys GMAC enhanced descriptor. IDF's PTP enable sets it for
// every frame; we set it only around our own PTP event frames.
#define TDES0_TTSE          (1u << 25)

// DMA status TS field: 4 = suspended (no descriptor owned by DMA), 0 = stopped.
#define DMA_TX_STATE_STOPPED    0u
#define DMA_TX_STATE_SUSPENDED  4u

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    _Atomic uint32_t range;     // (lo << 16) | hi; 0 = free
    net_udp_hook_t   fn;
    void            *ctx;
} hook_slot_t;

static esp_eth_handle_t s_eth;
static esp_eth_mac_t   *s_mac;
static esp_netif_t     *s_netif;
static uint8_t          s_mac_addr[6];
static atomic_bool      s_link;
static atomic_bool      s_ptp_ready;
static TaskHandle_t     s_maint_task;
static portMUX_TYPE     s_reg_lock = portMUX_INITIALIZER_UNLOCKED;

static hook_slot_t      s_hooks[MAX_HOOKS];
static _Atomic uint32_t s_groups[MAX_GROUPS];   // wanted multicast groups (net order), 0 = free
// 239/8 membership as a bitmap (exact: the 23-bit index encodes the group's
// last three octets bijectively). Kept next to s_groups; only 239/8 groups
// are mapped here — the RX fast path consults it for 239/8 frames only.
static _Atomic uint32_t s_group_bm[8];
static _Atomic uint32_t s_ip_id;

// RX counters have a single writer (the EMAC RX task); TX counters are bumped
// from several tasks, so they are atomic.
static volatile uint32_t s_rx_frames, s_rx_hooked, s_rx_to_lwip, s_rx_dropped, s_rx_mcast_filtered;
static _Atomic uint32_t s_tx_raw, s_tx_raw_fail;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static inline bool ip_is_multicast(uint32_t ip_net)
{
    const uint8_t *b = (const uint8_t *)&ip_net;
    return (b[0] & 0xF0u) == 0xE0u;
}

// 239.0.0.0/8: administratively scoped multicast, where AoIP audio flows live.
static inline bool ip_is_admin_scoped(uint32_t ip_net)
{
    return ((const uint8_t *)&ip_net)[0] == 239u;
}

static bool group_wanted(uint32_t g)
{
    // 239/8 only: the bitmap maps the group's 23 low bits exactly (net.h
    // net_group_bit). One relaxed load replaces a 32-slot scan in the RX
    // path under a multicast flood.
    const unsigned bit = net_group_bit(g);
    return (atomic_load_explicit(&s_group_bm[bit >> 5], memory_order_relaxed) >> (bit & 31u)) & 1u;
}

static struct netif *lwip_nif(void)
{
    return s_netif ? (struct netif *)esp_netif_get_netif_impl(s_netif) : NULL;
}

// ---------------------------------------------------------------------------
// Receive fast path (runs in IDF's emac_rx task, pinned to core 0)
// ---------------------------------------------------------------------------
static esp_err_t rx_input_body(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv, void *info)
{
    (void)eth;
    s_rx_frames++;

    if (len >= RAW_HDR_LEN && rd16(buf + 12) == ETHERTYPE_IPV4) {
        const uint8_t *ip = buf + ETH_HDR_LEN;
        uint32_t ihl = (uint32_t)(ip[0] & 0x0Fu) * 4u;
        uint32_t ip_total = rd16(ip + 2);
        uint16_t frag = rd16(ip + 6);
        if ((ip[0] >> 4) == 4u && ihl >= IP_HDR_LEN && ip[9] == IPPROTO_UDP_NUM &&
                (frag & 0x3FFFu) == 0 &&                            // not a fragment
                ip_total >= ihl + UDP_HDR_LEN && ETH_HDR_LEN + ip_total <= len) {
            const uint8_t *udp = ip + ihl;
            uint16_t dport = rd16(udp + 2);
            uint32_t udp_len = rd16(udp + 4);
            if (udp_len > ip_total - ihl) {
                udp_len = ip_total - ihl;
            }
            uint32_t dst_ip;
            memcpy(&dst_ip, ip + 16, 4);

            for (int i = 0; i < MAX_HOOKS; i++) {
                uint32_t r = atomic_load_explicit(&s_hooks[i].range, memory_order_acquire);
                if (r == 0 || dport < (r >> 16) || dport > (r & 0xFFFFu)) {
                    continue;
                }
                net_udp_meta_t meta;
                memcpy(&meta.src_ip, ip + 12, 4);
                meta.dst_ip = dst_ip;
                meta.src_port = rd16(udp);
                meta.dst_port = dport;
                meta.rx_ts_ns = 0;
                const eth_mac_time_t *ts = (const eth_mac_time_t *)info;
                if (ts && (ts->seconds | ts->nanoseconds)) {
                    meta.rx_ts_ns = (uint64_t)ts->seconds * 1000000000ull + ts->nanoseconds;
                }
                if (udp_len < UDP_HDR_LEN) {
                    break;
                }
                if (s_hooks[i].fn(udp + UDP_HDR_LEN, udp_len - UDP_HDR_LEN, &meta, s_hooks[i].ctx)) {
                    s_rx_hooked++;
                    free(buf);
                    return ESP_OK;
                }
                break;      // first matching range decides
            }

            // AoIP multicast audio we did not ask for: never let it reach
            // lwIP (on a network without IGMP snooping it would flood the
            // tcpip mailbox). Only 239/8 is filtered; control groups pass.
            if (ip_is_admin_scoped(dst_ip) && !group_wanted(dst_ip)) {
                s_rx_mcast_filtered++;
                free(buf);
                return ESP_OK;
            }
        }
    }

    s_rx_to_lwip++;
    esp_err_t err = esp_netif_receive((esp_netif_t *)priv, buf, len, NULL);
    if (err != ESP_OK) {
        s_rx_dropped++;
    }
    return err;
}

// Probe wrapper (docs/15): the whole fast path per frame, lwIP handoff
// included — it is the EMAC RX task's per-frame cost.
static esp_err_t rx_input(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv, void *info)
{
    perf_begin(PERF_RX_INPUT);
    esp_err_t err = rx_input_body(eth, buf, len, priv, info);
    perf_end(PERF_RX_INPUT);
    return err;
}

// lwIP asks the driver to add perfect multicast filters for each IGMP group.
// The EMAC has only a few slots, so we run the MAC in pass-all-multicast mode
// and filter in rx_input() instead; accept and ignore these requests.
static esp_err_t mac_filter_noop(void *h, const uint8_t *mac, size_t mac_len, bool add)
{
    (void)h; (void)mac; (void)mac_len; (void)add;
    return ESP_OK;
}

static void rx_free(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
esp_err_t net_udp_hook_register(uint16_t port_lo, uint16_t port_hi, net_udp_hook_t hook, void *ctx)
{
    ESP_RETURN_ON_FALSE(hook && port_lo != 0 && port_lo <= port_hi, ESP_ERR_INVALID_ARG, TAG, "bad hook");
    uint32_t r = ((uint32_t)port_lo << 16) | port_hi;
    esp_err_t ret = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_reg_lock);
    for (int i = 0; i < MAX_HOOKS; i++) {
        if (atomic_load_explicit(&s_hooks[i].range, memory_order_relaxed) == 0) {
            s_hooks[i].fn = hook;
            s_hooks[i].ctx = ctx;
            atomic_store_explicit(&s_hooks[i].range, r, memory_order_release);
            ret = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_reg_lock);
    return ret;
}

esp_err_t net_udp_hook_unregister(uint16_t port_lo, uint16_t port_hi)
{
    uint32_t r = ((uint32_t)port_lo << 16) | port_hi;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    portENTER_CRITICAL(&s_reg_lock);
    for (int i = 0; i < MAX_HOOKS; i++) {
        if (r != 0 && atomic_load_explicit(&s_hooks[i].range, memory_order_relaxed) == r) {
            atomic_store_explicit(&s_hooks[i].range, 0, memory_order_release);
            ret = ESP_OK;
            break;
        }
    }
    portEXIT_CRITICAL(&s_reg_lock);
    return ret;
}

// ---------------------------------------------------------------------------
// Multicast membership. lwIP calls must run in the tcpip thread.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t group;
    bool     join;
} mcast_req_t;

static esp_err_t tcpip_mcast(void *arg)
{
    const mcast_req_t *rq = arg;
    struct netif *n = lwip_nif();
    if (!n || !netif_is_up(n)) {
        return ESP_ERR_INVALID_STATE;
    }
    ip4_addr_t g;
    ip4_addr_set_u32(&g, rq->group);
    if (rq->join) {
        if (igmp_lookfor_group(n, &g) == NULL && igmp_joingroup_netif(n, &g) != ERR_OK) {
            return ESP_FAIL;
        }
    } else if (igmp_lookfor_group(n, &g) != NULL) {
        igmp_leavegroup_netif(n, &g);
    }
    return ESP_OK;
}

// Re-join every wanted group lwIP has lost (link loss frees memberships).
static esp_err_t tcpip_mcast_ensure(void *arg)
{
    (void)arg;
    struct netif *n = lwip_nif();
    if (!n || !netif_is_up(n) || !netif_is_link_up(n)) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < MAX_GROUPS; i++) {
        uint32_t gv = atomic_load_explicit(&s_groups[i], memory_order_relaxed);
        if (gv == 0) {
            continue;
        }
        ip4_addr_t g;
        ip4_addr_set_u32(&g, gv);
        if (igmp_lookfor_group(n, &g) == NULL) {
            igmp_joingroup_netif(n, &g);
        }
    }
    return ESP_OK;
}

esp_err_t net_mcast_join(uint32_t group_ip)
{
    ESP_RETURN_ON_FALSE(ip_is_multicast(group_ip), ESP_ERR_INVALID_ARG, TAG, "not multicast");
    bool have = false;
    portENTER_CRITICAL(&s_reg_lock);
    int free_slot = -1;
    for (int i = 0; i < MAX_GROUPS; i++) {
        uint32_t v = atomic_load_explicit(&s_groups[i], memory_order_relaxed);
        if (v == group_ip) {
            have = true;
            break;
        }
        if (v == 0 && free_slot < 0) {
            free_slot = i;
        }
    }
    if (!have && free_slot >= 0) {
        atomic_store_explicit(&s_groups[free_slot], group_ip, memory_order_release);
        have = true;
        if (ip_is_admin_scoped(group_ip)) {
            const unsigned bit = net_group_bit(group_ip);
            atomic_fetch_or_explicit(&s_group_bm[bit >> 5], 1u << (bit & 31u), memory_order_relaxed);
        }
    }
    portEXIT_CRITICAL(&s_reg_lock);
    ESP_RETURN_ON_FALSE(have, ESP_ERR_NO_MEM, TAG, "group table full");

    // Join now if the interface is up; otherwise the maintenance task joins
    // as soon as it is (and again after any link loss).
    mcast_req_t rq = { .group = group_ip, .join = true };
    if (s_netif) {
        esp_netif_tcpip_exec(tcpip_mcast, &rq);
    }
    return ESP_OK;
}

esp_err_t net_mcast_leave(uint32_t group_ip)
{
    bool found = false;
    portENTER_CRITICAL(&s_reg_lock);
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (atomic_load_explicit(&s_groups[i], memory_order_relaxed) == group_ip) {
            atomic_store_explicit(&s_groups[i], 0, memory_order_release);
            found = true;
        }
    }
    portEXIT_CRITICAL(&s_reg_lock);
    if (found) {
        if (ip_is_admin_scoped(group_ip)) {
            const unsigned bit = net_group_bit(group_ip);
            atomic_fetch_and_explicit(&s_group_bm[bit >> 5], ~(1u << (bit & 31u)), memory_order_relaxed);
        }
        if (s_netif) {
            mcast_req_t rq = { .group = group_ip, .join = false };
            esp_netif_tcpip_exec(tcpip_mcast, &rq);
        }
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Raw UDP transmit
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t dst;       // net order
    uint8_t  mac[6];
    bool     ok;
} arp_req_t;

static esp_err_t tcpip_arp(void *arg)
{
    arp_req_t *rq = arg;
    struct netif *n = lwip_nif();
    rq->ok = false;
    if (!n || !netif_is_up(n) || !netif_is_link_up(n)) {
        return ESP_ERR_INVALID_STATE;
    }
    ip4_addr_t target;
    ip4_addr_set_u32(&target, rq->dst);
    const ip4_addr_t *me = netif_ip4_addr(n);
    const ip4_addr_t *mask = netif_ip4_netmask(n);
    const ip4_addr_t *gw = netif_ip4_gw(n);
    // Off-subnet (and not link-local): go through the gateway.
    if (!ip4_addr_isany(me) && !ip4_addr_islinklocal(&target) &&
            !ip4_addr_net_eq(&target, me, mask) && !ip4_addr_isany(gw)) {
        target = *gw;
    }
    struct eth_addr *eth_ret = NULL;
    const ip4_addr_t *ip_ret = NULL;
    if (etharp_find_addr(n, &target, &eth_ret, &ip_ret) >= 0 && eth_ret) {
        memcpy(rq->mac, eth_ret->addr, 6);
        rq->ok = true;
    } else {
        etharp_query(n, &target, NULL);     // sends a request, does not wait
    }
    return ESP_OK;
}

esp_err_t net_udp_dest_resolve(net_udp_dest_t *d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "null dest");
    const uint8_t *b = (const uint8_t *)&d->dst_ip;
    if (ip_is_multicast(d->dst_ip)) {
        d->dst_mac[0] = 0x01; d->dst_mac[1] = 0x00; d->dst_mac[2] = 0x5E;
        d->dst_mac[3] = b[1] & 0x7Fu; d->dst_mac[4] = b[2]; d->dst_mac[5] = b[3];
        d->resolved = true;
        return ESP_OK;
    }
    if (d->dst_ip == 0xFFFFFFFFu) {
        memset(d->dst_mac, 0xFF, 6);
        d->resolved = true;
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_netif, ESP_ERR_INVALID_STATE, TAG, "net not started");
    arp_req_t rq = { .dst = d->dst_ip };
    esp_err_t err = esp_netif_tcpip_exec(tcpip_arp, &rq);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (!rq.ok) {
        return ESP_ERR_NOT_FINISHED;
    }
    memcpy(d->dst_mac, rq.mac, 6);
    d->resolved = true;
    net_udp_dest_seal(d);
    return ESP_OK;
}

static void build_headers(uint8_t *h, const net_udp_dest_t *d, size_t len)
{
    // Ethernet
    memcpy(h, d->dst_mac, 6);
    memcpy(h + 6, s_mac_addr, 6);
    wr16(h + 12, ETHERTYPE_IPV4);
    // IPv4
    uint8_t *ip = h + ETH_HDR_LEN;
    uint32_t src = net_ipv4();
    ip[0] = 0x45;
    ip[1] = (uint8_t)(d->dscp << 2);
    wr16(ip + 2, (uint16_t)(IP_HDR_LEN + UDP_HDR_LEN + len));
    wr16(ip + 4, (uint16_t)atomic_fetch_add_explicit(&s_ip_id, 1, memory_order_relaxed));
    wr16(ip + 6, 0x4000);                               // DF, no fragments
    ip[8] = ip_is_multicast(d->dst_ip) ? 32 : 64;       // TTL
    ip[9] = IPPROTO_UDP_NUM;
    ip[10] = 0; ip[11] = 0;
    memcpy(ip + 12, &src, 4);
    memcpy(ip + 16, &d->dst_ip, 4);
    uint32_t sum = 0;
    for (int i = 0; i < (int)IP_HDR_LEN; i += 2) {
        sum += rd16(ip + i);
    }
    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    wr16(ip + 10, (uint16_t)~sum);
    // UDP (checksum 0 = none, legal for IPv4)
    uint8_t *udp = ip + IP_HDR_LEN;
    wr16(udp, d->src_port);
    wr16(udp + 2, d->dst_port);
    wr16(udp + 4, (uint16_t)(UDP_HDR_LEN + len));
    udp[6] = 0; udp[7] = 0;
}

// Bake the constant part of the wire header for this destination. The three
// variable words (IP total length, IP ID, UDP length) are ZEROED in the
// template and in the base sum: the send path adds exactly those (and folds),
// so the wire checksum is ~fold2(chk_base + total + udp_len + id) — identical
// to recomputing the whole fold per packet.
bool net_udp_dest_seal(net_udp_dest_t *d)
{
    if (!d->resolved) return false;
    uint32_t old_src;
    memcpy(&old_src, d->hdr + ETH_HDR_LEN + 12, 4);
    bool changed = !d->sealed || old_src != net_ipv4();
    build_headers(d->hdr, d, 0);
    wr16(d->hdr + ETH_HDR_LEN + 2, 0);              // IP total length
    wr16(d->hdr + ETH_HDR_LEN + 4, 0);              // IP ID
    wr16(d->hdr + ETH_HDR_LEN + IP_HDR_LEN + 4, 0); // UDP length
    uint32_t sum = 0;
    for (unsigned i = 0; i < NET_RAW_HDR_LEN; i += 2) {
        if (i == ETH_HDR_LEN + 10) continue;        // the checksum word itself
        sum += rd16(d->hdr + i);
    }
    d->chk_base = sum;
    d->sealed = true;
    return changed;
}

// Head from the template, or a live build when it is not sealed yet. The
// template was built for a zero-length payload: patch total length, IP ID,
// UDP length and the checksum (linear fold over the constant base).
static void send_head(const net_udp_dest_t *d, size_t len, uint8_t hdr[RAW_HDR_LEN])
{
    if (!d->sealed) {
        build_headers(hdr, d, len);
        return;
    }
    memcpy(hdr, d->hdr, RAW_HDR_LEN);
    const uint16_t id = (uint16_t)atomic_fetch_add_explicit(&s_ip_id, 1, memory_order_relaxed);
    const uint16_t total = (uint16_t)(IP_HDR_LEN + UDP_HDR_LEN + len);
    const uint16_t udp_len = (uint16_t)(UDP_HDR_LEN + len);
    wr16(hdr + ETH_HDR_LEN + 2, total);
    wr16(hdr + ETH_HDR_LEN + 4, id);
    wr16(hdr + ETH_HDR_LEN + IP_HDR_LEN + 4, udp_len);
    uint32_t raw = d->chk_base + (uint32_t)total + (uint32_t)udp_len + (uint32_t)id;
    raw = (raw & 0xFFFFu) + (raw >> 16);
    raw = (raw & 0xFFFFu) + (raw >> 16);
    wr16(hdr + ETH_HDR_LEN + 10, (uint16_t)~raw);
}

esp_err_t net_udp_send_raw(const net_udp_dest_t *d, const void *payload, size_t len)
{
    if (!d || !d->resolved || len > RAW_MAX_PAYLOAD || (len && !payload) || !s_eth) {
        atomic_fetch_add_explicit(&s_tx_raw_fail, 1, memory_order_relaxed);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hdr[RAW_HDR_LEN];
    send_head(d, len, hdr);
    esp_err_t err;
    if (len) {
        err = esp_eth_transmit_ctrl_vargs(s_eth, NULL, 4, hdr, (uint32_t)RAW_HDR_LEN,
                                          (uint8_t *)payload, (uint32_t)len);
    } else {
        err = esp_eth_transmit_ctrl_vargs(s_eth, NULL, 2, hdr, (uint32_t)RAW_HDR_LEN);
    }
    atomic_fetch_add_explicit(err == ESP_OK ? &s_tx_raw : &s_tx_raw_fail, 1, memory_order_relaxed);
    return err;
}

esp_err_t net_udp_send_raw_ts(const net_udp_dest_t *d, const void *payload, size_t len, uint64_t *tx_ts_ns)
{
    *tx_ts_ns = 0;
    net_udp_dest_seal((net_udp_dest_t *)d);     // keep the source address current
    if (!d || !d->resolved || len == 0 || len > RAW_MAX_PAYLOAD || !payload || !s_eth) {
        atomic_fetch_add_explicit(&s_tx_raw_fail, 1, memory_order_relaxed);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hdr[RAW_HDR_LEN];
    send_head(d, len, hdr);

    // Let the TX DMA drain (bounded) so our frame is next on the wire and the
    // driver's bounded wait for the timestamp write-back is long enough.
    for (int i = 0; i < 200; i++) {
        uint32_t st = EMAC_DMA.dmastatus.trans_proc_state;
        if (st == DMA_TX_STATE_SUSPENDED || st == DMA_TX_STATE_STOPPED) {
            break;
        }
        esp_rom_delay_us(5);
    }

    uint32_t ttse = TDES0_TTSE;
    eth_mac_time_t ts = { 0 };
    esp_eth_ioctl(s_eth, (esp_eth_io_cmd_t)ETH_MAC_ESP_CMD_SET_TDES0_CFG_BITS, &ttse);
    esp_err_t err = esp_eth_transmit_ctrl_vargs(s_eth, &ts, 4, hdr, (uint32_t)RAW_HDR_LEN,
                                                (uint8_t *)payload, (uint32_t)len);
    esp_eth_ioctl(s_eth, (esp_eth_io_cmd_t)ETH_MAC_ESP_CMD_CLEAR_TDES0_CFG_BITS, &ttse);
    atomic_fetch_add_explicit(err == ESP_OK ? &s_tx_raw : &s_tx_raw_fail, 1, memory_order_relaxed);
    if (err == ESP_OK && (ts.seconds | ts.nanoseconds)) {
        *tx_ts_ns = (uint64_t)ts.seconds * 1000000000ull + ts.nanoseconds;
    }
    return err;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
esp_netif_t *net_netif(void) { return s_netif; }
bool net_link_up(void) { return atomic_load(&s_link); }
void net_mac(uint8_t mac[6]) { esp_read_mac(mac, ESP_MAC_ETH); }
esp_eth_handle_t net_eth_handle(void) { return s_eth; }
bool net_ptp_hw_ready(void) { return atomic_load(&s_ptp_ready); }

uint32_t net_ipv4(void)
{
    // Read lwIP's address directly: a link-local (AutoIP) address never posts
    // IP_EVENT_ETH_GOT_IP. A 32-bit aligned load is atomic.
    struct netif *n = lwip_nif();
    if (!n) {
        return 0;
    }
    return ip4_addr_get_u32(netif_ip4_addr(n));
}

void net_get_stats(net_stats_t *s)
{
    s->rx_frames = s_rx_frames;
    s->rx_hooked = s_rx_hooked;
    s->rx_to_lwip = s_rx_to_lwip;
    s->rx_dropped = s_rx_dropped;
    s->tx_raw = atomic_load_explicit(&s_tx_raw, memory_order_relaxed);
    s->tx_raw_fail = atomic_load_explicit(&s_tx_raw_fail, memory_order_relaxed);
    s->rx_mcast_filtered = s_rx_mcast_filtered;
    // Free RX descriptors: DMA-ring pressure at burst rates. The value is
    // updated by the EMAC RX task between frames; a relaxed read from the
    // telemetry task is good enough.
    s->rx_dma_free_desc = s_mac ? esp_eth_mac_esp_free_rx_desc(s_mac) : 0;
}

// ---------------------------------------------------------------------------
// EMAC / PHY register tweaks
// ---------------------------------------------------------------------------

// Hold (do not flush) received frames when no RX descriptor is free. With
// flushing, a timestamped PTP frame flushed during a burst has been seen to
// wedge the MTL RX FIFO read controller for good (see docs/11-net-clock.md).
static void emac_rx_hold_frames(void)
{
    EMAC_DMA.dmaoperation_mode.dis_flush_recv_frames = 1;
}

// IEEE 1588 unit: timestamp only PTPv1 event messages carried in UDP/IPv4
// (Sync, Delay_Req), never every frame.
static void emac_ptp_classifier_v1(void)
{
    portENTER_CRITICAL(&s_reg_lock);
    EMAC_PTP.timestamp_ctrl.en_ts4all = 0;
    EMAC_PTP.timestamp_ctrl.en_ptp_pkg_proc_ver2_fmt = 0;
    EMAC_PTP.timestamp_ctrl.en_proc_ptp_ether_frm = 0;
    EMAC_PTP.timestamp_ctrl.en_proc_ptp_ipv6_udp = 0;
    EMAC_PTP.timestamp_ctrl.en_proc_ptp_ipv4_udp = 1;
    EMAC_PTP.timestamp_ctrl.en_ts_snap_event_msg = 1;
    EMAC_PTP.timestamp_ctrl.en_snap_msg_relevant_master = 0;
    EMAC_PTP.timestamp_ctrl.sel_snap_type = 0;
    EMAC_PTP.timestamp_ctrl.en_mac_addr_filter = 0;
    portEXIT_CRITICAL(&s_reg_lock);
}

static esp_err_t phy_wr(uint32_t reg, uint32_t val)
{
    esp_eth_phy_reg_rw_data_t d = { .reg_addr = reg, .reg_value_p = &val };
    return esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &d);
}

static esp_err_t phy_rd(uint32_t reg, uint32_t *val)
{
    esp_eth_phy_reg_rw_data_t d = { .reg_addr = reg, .reg_value_p = val };
    return esp_eth_ioctl(s_eth, ETH_CMD_READ_PHY_REG, &d);
}

// Energy Efficient Ethernet makes the link sleep between frames; the wake-up
// adds tens of µs to a lone Sync but not under load, so the path delay would
// depend on traffic. Clear our EEE advertisement (MMD 7, reg 60, via the
// clause-22 indirect registers 13/14) and restart autonegotiation. A PHY
// without EEE ignores this.
static void phy_disable_eee(void)
{
    esp_err_t e = ESP_OK;
    e |= phy_wr(13, 0x0007);            // MMD 7, function: address
    e |= phy_wr(14, 60);                // EEE advertisement register
    e |= phy_wr(13, 0x4007);            // MMD 7, function: data
    e |= phy_wr(14, 0x0000);            // advertise nothing
    uint32_t bmcr = 0;
    e |= phy_rd(0, &bmcr);
    e |= phy_wr(0, bmcr | 0x1200u);     // AN enable + restart AN
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "EEE disable: PHY register access failed");
    }
}

// ---------------------------------------------------------------------------
// Events and maintenance
// ---------------------------------------------------------------------------
static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        atomic_store(&s_link, true);
        emac_rx_hold_frames();
        ESP_LOGI(TAG, "link up");
        if (s_maint_task) {
            xTaskNotifyGive(s_maint_task);
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        atomic_store(&s_link, false);
        ESP_LOGW(TAG, "link down");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP && s_maint_task) {
        xTaskNotifyGive(s_maint_task);
    }
}

static void maint_task(void *arg)
{
    uint32_t last_ip = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (!atomic_load(&s_link)) {
            continue;
        }
        esp_netif_tcpip_exec(tcpip_mcast_ensure, NULL);
        uint32_t ip = net_ipv4();
        if (ip != last_ip) {
            const uint8_t *b = (const uint8_t *)&ip;
            ESP_LOGI(TAG, "IPv4 %u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            last_ip = ip;
        }
    }
}

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------
static esp_err_t net_bringup(void)
{
    esp_err_t err = esp_netif_init();
    ESP_RETURN_ON_ERROR(err, TAG, "esp_netif_init");
    err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");

    ESP_ERROR_CHECK(esp_read_mac(s_mac_addr, ESP_MAC_ETH));

    // MAC: the RX task is pinned to the installing core (this one: core 0).
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = EMAC_RX_STACK;
    mac_cfg.rx_task_prio = UI_PRIO_EMAC_RX;
    mac_cfg.flags |= ETH_MAC_FLAG_PIN_TO_CORE;

    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = UI_PIN_ETH_MDC;
    emac_cfg.smi_gpio.mdio_num = UI_PIN_ETH_MDIO;
    emac_cfg.interface = EMAC_DATA_INTERFACE_RMII;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = UI_PIN_ETH_REF_CLK;
    emac_cfg.emac_dataif_gpio.rmii.tx_en_num = UI_PIN_ETH_TX_EN;
    emac_cfg.emac_dataif_gpio.rmii.txd0_num = UI_PIN_ETH_TXD0;
    emac_cfg.emac_dataif_gpio.rmii.txd1_num = UI_PIN_ETH_TXD1;
    emac_cfg.emac_dataif_gpio.rmii.crs_dv_num = UI_PIN_ETH_CRS_DV;
    emac_cfg.emac_dataif_gpio.rmii.rxd0_num = UI_PIN_ETH_RXD0;
    emac_cfg.emac_dataif_gpio.rmii.rxd1_num = UI_PIN_ETH_RXD1;
    s_mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    ESP_RETURN_ON_FALSE(s_mac, ESP_FAIL, TAG, "EMAC create failed");

    // PHY: IP101 through IDF's generic IEEE 802.3 driver (IDF v6 moved the
    // chip drivers to the component registry; the IP101 needs nothing the
    // generic driver lacks for 100M full duplex with autonegotiation).
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = UI_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = UI_PIN_ETH_PHY_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    ESP_RETURN_ON_FALSE(phy, ESP_FAIL, TAG, "PHY create failed");

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(s_mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &s_eth), TAG, "driver install");

    // No 802.3x pause: a receive stall must not freeze the switch port.
    bool off = false;
    esp_eth_ioctl(s_eth, ETH_CMD_S_FLOW_CTRL, &off);
    // Receive every multicast frame; rx_input() and lwIP filter by group.
    bool on = true;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_S_ALL_MULTICAST, &on), TAG, "all-multicast");
    phy_disable_eee();
    emac_rx_hold_frames();

    // esp_netif: DHCP client with AutoIP (link-local) fallback (sdkconfig).
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&ncfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif create failed");
    char host[32];
    snprintf(host, sizeof host, "%s%02X%02X%02X", UI_NAME_PREFIX,
             s_mac_addr[3], s_mac_addr[4], s_mac_addr[5]);
    esp_netif_set_hostname(s_netif, host);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)), TAG, "netif attach");

    // The glue installed its own input path during attach; put ours in front.
    ESP_RETURN_ON_ERROR(esp_eth_update_input_path_info(s_eth, rx_input, s_netif), TAG, "input path");
    esp_netif_driver_ifconfig_t drv = {
        .handle = s_eth,
        .transmit = esp_eth_transmit,
        .driver_free_rx_buffer = rx_free,
        .driver_set_mac_filter = mac_filter_noop,
    };
    ESP_RETURN_ON_ERROR(esp_netif_set_driver_config(s_netif, &drv), TAG, "driver config");

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL));

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth), TAG, "eth start");

    // IEEE 1588: XTAL (40 MHz) clocks the counter, 40 ns per tick, digital
    // rollover (the sub-second register counts ns), fine (addend) update.
    eth_mac_ptp_config_t ptp_cfg = ETH_MAC_ESP_PTP_DEFAULT_CONFIG();
    ptp_cfg.clk_src = EMAC_PTP_CLK_SRC_XTAL;
    ptp_cfg.clk_src_period_ns = 25;
    ptp_cfg.required_accuracy_ns = 40;
    ptp_cfg.roll_type = ETH_PTP_DIGITAL_ROLLOVER;
    ESP_RETURN_ON_ERROR(esp_eth_mac_ptp_enable(s_mac, &ptp_cfg), TAG, "PTP enable");
    emac_ptp_classifier_v1();
    // IDF's PTP enable requests a TX timestamp for every frame; we only want
    // them for our own Delay_Req (net_udp_send_raw_ts sets it around it).
    uint32_t ttse = TDES0_TTSE;
    esp_eth_ioctl(s_eth, (esp_eth_io_cmd_t)ETH_MAC_ESP_CMD_CLEAR_TDES0_CFG_BITS, &ttse);
    atomic_store(&s_ptp_ready, true);

    BaseType_t ok = xTaskCreatePinnedToCore(maint_task, "net_maint", 3072, NULL,
                                            UI_PRIO_CONTROL, &s_maint_task, UI_CORE_USB);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "maint task");

    ESP_LOGI(TAG, "EMAC up: %02x:%02x:%02x:%02x:%02x:%02x, host %s",
             s_mac_addr[0], s_mac_addr[1], s_mac_addr[2], s_mac_addr[3], s_mac_addr[4], s_mac_addr[5], host);
    return ESP_OK;
}

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t         err;
} start_ctx_t;

static void start_task(void *arg)
{
    start_ctx_t *c = arg;
    c->err = net_bringup();
    xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

esp_err_t net_start(void)
{
    if (s_eth) {
        return ESP_OK;
    }
    // ETH_MAC_FLAG_PIN_TO_CORE pins the EMAC RX task to the installing core,
    // so install from UI_CORE_NET whatever core the caller is on.
    if (esp_cpu_get_core_id() == UI_CORE_NET) {
        return net_bringup();
    }
    start_ctx_t c = { .done = xSemaphoreCreateBinary(), .err = ESP_FAIL };
    ESP_RETURN_ON_FALSE(c.done, ESP_ERR_NO_MEM, TAG, "sem");
    if (xTaskCreatePinnedToCore(start_task, "net_start", 6144, &c, UI_PRIO_CONTROL,
                                NULL, UI_CORE_NET) != pdPASS) {
        vSemaphoreDelete(c.done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(c.done, portMAX_DELAY);
    vSemaphoreDelete(c.done);
    return c.err;
}
