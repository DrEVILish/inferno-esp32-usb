// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal interfaces between the AoIP modules. Not for use outside aoip/.
//
// Threads:
//   ctl task  (core 1, UI_PRIO_CONTROL): ARC, CMC, info/heartbeat, flow
//             control server, TX flow table, TX multicasts, RX keepalives,
//             NVS flushes, IP change handling.
//   sub task  (core 1, UI_PRIO_CONTROL - 1): channel subscriber (mDNS
//             queries and flow requests, which block for seconds).
//   packetizer (core 0, UI_PRIO_TX_PACKETIZER): builds and sends TX audio.
//   EMAC RX task (core 0, net/): calls our UDP hooks (RX audio, TX keepalives).
// Shared control state is protected by aoip_lock(); the audio paths use only
// lock-free published snapshots (see flows_tx.c, flows_rx.c).
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_config.h"
#include "aoip/aoip.h"

#define AOIP_NAME_LEN           32u     // names: max 31 chars + NUL (Dante limit)
#define AOIP_TX_FLOW_SLOTS      UI_TX_MAX_FLOWS
#define AOIP_RX_FLOW_SLOTS      UI_RX_MAX_FLOWS
#define AOIP_RX_MAX_CH_PER_FLOW 16u     // channels in a flow we receive (bundles may exceed 8)
#define AOIP_BOARD_NAME         UI_MODEL_ID

// ---------------------------------------------------------------------------
// Device state (aoip.c)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  mac[6];
    uint8_t  devid[8];                  // factory_device_id: EUI-64 of the MAC
    uint16_t process_id;                // always 0
    char factory_hostname[48];
    char default_hostname[AOIP_NAME_LEN];
    char friendly_hostname[AOIP_NAME_LEN];
    char tx_names[UI_CHANNELS][AOIP_NAME_LEN];   // friendly names
    char rx_names[UI_CHANNELS][AOIP_NAME_LEN];
    _Atomic uint32_t ip;                // network byte order, 0 = none
    uint32_t netmask, gw;               // network byte order
} aoip_dev_t;

extern aoip_dev_t g_aoip;

void aoip_lock(void);                   // recursive
void aoip_unlock(void);
void aoip_factory_ch_name(char out[4], unsigned index);   // "01".."32"
uint32_t aoip_now_ms(void);

// Counters (aoip_get_stats). Writers: any module; relaxed atomics.
typedef struct {
    _Atomic uint32_t tx_packets, tx_send_fail, tx_underruns, tx_lag_resets;
    _Atomic uint32_t rx_packets, rx_late, rx_early, rx_seq_gaps, rx_wrong_len;
    _Atomic uint32_t rx_ka_sent, rx_ka_fail;
    _Atomic uint32_t fc_requests, fc_failures;
    _Atomic uint32_t arc_requests;
    _Atomic uint32_t nvs_writes;
    _Atomic uint32_t info_drops;    // info requests refused by the rate limit
} aoip_counters_t;
extern aoip_counters_t g_aoip_ctr;
#define CTR_INC(f) atomic_fetch_add_explicit(&g_aoip_ctr.f, 1u, memory_order_relaxed)

// ---------------------------------------------------------------------------
// Request/response servers (ctl task). Each returns the response length
// (header included, written to `out`) or 0 for no response.
// ---------------------------------------------------------------------------
size_t arc_handle(const uint8_t *req, size_t len, uint8_t *out, size_t cap);
size_t cmc_handle(const uint8_t *req, size_t len, uint8_t *out, size_t cap);
size_t fcs_handle(const uint8_t *req, size_t len, uint8_t *out, size_t cap);

// ---------------------------------------------------------------------------
// Info multicasts + heartbeat (info_mcast.c, ctl task unless noted)
// ---------------------------------------------------------------------------
void info_set_socket(int sock);
void info_on_request(const uint8_t *pkt, size_t len);
void info_send_board_and_product(void);
void info_heartbeat(void);
// Channel status change notification for RX channel indices in `mask`.
// Any task; queued and sent from the ctl task.
void info_notify_rx_channels(uint32_t mask);
void info_poll(void);

// ---------------------------------------------------------------------------
// mDNS (aoip_mdns.c)
// ---------------------------------------------------------------------------
esp_err_t aoip_mdns_init(void);
void aoip_mdns_ip_changed(void);        // (re)enable + announce on the netif
void aoip_mdns_publish_all(void);       // device services + all TX channels
void aoip_mdns_request_republish(void);   // defer publish_all past the serve() lock
void aoip_mdns_publish_if_pending(void);  // ctl loop: republish after unlock
void aoip_mdns_refresh_tx_channel(unsigned index);
void aoip_mdns_remove_tx_channel_name(unsigned index, const char *name);
void aoip_mdns_add_bundle(unsigned bundle_id, unsigned nch, unsigned fpp,
                          uint32_t ip, uint16_t port);
void aoip_mdns_remove_bundle(unsigned bundle_id);

typedef struct {
    uint32_t ip;                        // network byte order
    uint16_t port;
    uint16_t nchan, tx_channel_id, dbcp1, fpp_min, fpp_max;
    uint32_t bits;
    uint32_t min_latency_ns;
    bool     has_bundle;
    uint16_t bundle_id, channel_in_bundle;   // channel_in_bundle 0-based
} aoip_adv_chan_t;

typedef struct {
    uint32_t ip;                        // media address (multicast), network order
    uint16_t port;
    uint16_t nchan, bundle_id, fpp;
    uint32_t bits;
    uint32_t min_latency_ns;
} aoip_adv_bundle_t;

// Resolve n "<chan>@<host>" services in parallel. ok[i] tells success.
// Blocking (up to ~4 s). Subscriber task only.
void aoip_mdns_query_chans(const char (*host)[AOIP_NAME_LEN], const char (*chan)[AOIP_NAME_LEN],
                           size_t n, aoip_adv_chan_t *out, bool *ok);
bool aoip_mdns_query_bundle(const char *full_name, aoip_adv_bundle_t *out);

// ---------------------------------------------------------------------------
// Persistence (settings_nvs.c)
// ---------------------------------------------------------------------------
#define SET_DIRTY_NAMES   (1u << 0)     // device name + channel names
#define SET_DIRTY_SUBS    (1u << 1)
#define SET_DIRTY_TXMC    (1u << 2)

typedef struct {
    char txch[AOIP_NAME_LEN];
    char host[AOIP_NAME_LEN];
} saved_sub_t;                          // txch[0] == 0: not subscribed

typedef struct {
    uint8_t  flow_id;                   // 0 = unused
    uint8_t  nch;
    uint8_t  ch_ids[UI_TX_MAX_CH_PER_FLOW];   // 1-based, 0 = none
    uint32_t dst_ip;                    // network order
} saved_txmc_t;

void settings_load(void);               // names into g_aoip; subs/txmc kept for their owners
void settings_get_subs(saved_sub_t out[UI_CHANNELS]);
void settings_get_txmc(saved_txmc_t out[AOIP_TX_FLOW_SLOTS]);
void settings_set_subs(const saved_sub_t subs[UI_CHANNELS]);     // marks dirty
void settings_set_txmc(const saved_txmc_t mc[AOIP_TX_FLOW_SLOTS]); // marks dirty
void settings_mark_dirty(uint32_t bits);
void settings_flush_if_due(void);       // ctl task: write 2 s after the last change, only if changed
uint32_t settings_skipped_flushes(void);  // quiet periods that found nothing new to write

// ---------------------------------------------------------------------------
// TX flows (flows_tx.c). Control functions: ctl task, under aoip_lock.
// ---------------------------------------------------------------------------
typedef struct {
    bool     used;
    bool     multicast;
    bool     expired;
    char     rx_hostname[AOIP_NAME_LEN];
    char     rx_flow_name[AOIP_NAME_LEN];
    uint32_t dst_ip;                    // network order
    uint16_t dst_port;
    uint8_t  nch;
    int8_t   ch[UI_TX_MAX_CH_PER_FLOW]; // local TX channel index, -1 = none
    uint16_t fpp;
    uint8_t  bps;                       // bytes per sample 2/3/4
    uint16_t cookie;
} ftx_info_t;

esp_err_t ftx_init(void);
// Returns ESP_OK and fills index/handle; errors as in flows_tx.rs add_flow.
esp_err_t ftx_add_flow(const ftx_info_t *info, int requested_index, bool is_multicast,
                       unsigned *index_out, uint8_t handle_out[6]);
esp_err_t ftx_remove_flow(const uint8_t handle[6]);
esp_err_t ftx_remove_multicast_flow(unsigned index);
esp_err_t ftx_set_channels(const uint8_t handle[6], const int8_t *ch, unsigned n);
void ftx_activate_multicast_flow(unsigned index);
bool ftx_destination_exists(uint32_t ip, uint16_t port);
void ftx_random_multicast_destination(uint32_t *ip, uint16_t *port);
const ftx_info_t *ftx_get(unsigned index);   // NULL if slot unused
void ftx_tick(void);                    // expiry, ARP resolution, publish
unsigned ftx_active_count(void);

// ---------------------------------------------------------------------------
// TX multicasts (tx_multicasts.c, ctl task)
// ---------------------------------------------------------------------------
void txm_load_state(void);
bool txm_add_flow(unsigned flow_index, const int8_t *ch, unsigned n);
esp_err_t txm_remove_flow(unsigned flow_index);
void txm_tick(void);
// mDNS "b.<bundle>=<channel_in_bundle+1>" for a TX channel; false if none.
bool txm_channel_bundle(unsigned tx_ch, unsigned *bundle_id, unsigned *ch_in_bundle);
void txm_mdns_republish(void);           // re-add active bundles after publish_all

// ---------------------------------------------------------------------------
// RX flows (flows_rx.c)
// ---------------------------------------------------------------------------
typedef struct {
    bool     used;
    uint16_t rx_port;
    uint8_t  nch;
    uint32_t channels_map[AOIP_RX_MAX_CH_PER_FLOW];  // bit = local RX channel
    uint32_t latency_frames;
} frx_info_t;

esp_err_t frx_init(void);
uint16_t frx_port(unsigned slot);
// Activate a slot. mcast_ip != 0: multicast flow on AOIP_PORT_MEDIA_MCAST.
// Opens the keepalive socket for unicast flows. sub task, may block briefly.
int32_t ftx_take_lag_max(void);                   // telemetry window (INT32_MIN = none)
esp_err_t frx_add_flow(unsigned slot, uint32_t mcast_ip, unsigned bps, unsigned nch,
                       uint32_t latency_frames, bool send_keepalives);
void frx_remove_flow(unsigned slot);
void frx_connect_channel(unsigned slot, unsigned ch_in_flow, unsigned local_ch);
void frx_disconnect_channel(unsigned slot, unsigned ch_in_flow, unsigned local_ch);
uint32_t frx_last_packet_ms(unsigned slot);       // 0 if never
bool frx_get_info(unsigned slot, frx_info_t *out);
int32_t frx_take_max_latency(unsigned slot);
int32_t frx_take_min_margin(unsigned slot);       // telemetry window (INT32_MAX = none)      // heartbeat 0x8003 (swap 0)
void frx_keepalive_tick(void);                    // ctl task
void frx_take_peaks(uint32_t peaks[UI_CHANNELS]); // max |sample| since last call
unsigned frx_active_count(void);

// ---------------------------------------------------------------------------
// Subscriber (subscriber.c)
// ---------------------------------------------------------------------------
typedef struct {
    bool     present;
    char     txch[AOIP_NAME_LEN];
    char     host[AOIP_NAME_LEN];
    uint32_t status;                    // SUB_STATUS_*
} sub_info_t;

esp_err_t sub_start(void);                     // idempotent; retryable after RC failure
bool sub_subscribe(unsigned local_ch, const char *txch, const char *host); // ctl task; false = not enqueued
bool sub_unsubscribe(unsigned local_ch);                                   // ctl task; false = not enqueued
void sub_get_info(unsigned local_ch, sub_info_t *out);
void sub_counts(uint32_t *total, uint32_t *ok);

// ---------------------------------------------------------------------------
// Flow control client (flowctl_client.c, sub task)
// ---------------------------------------------------------------------------
// Returns 1 (OK), the transmitter's error code, or 0xFFFF on timeout/IO error.
uint16_t fcc_request_flow(uint32_t ip, uint16_t port, uint32_t rate, uint32_t bits, uint16_t fpp,
                          const uint16_t *tx_ch, unsigned n, uint16_t rx_port,
                          const char *rx_flow_name, uint8_t handle_out[6]);
uint16_t fcc_update_flow(uint32_t ip, uint16_t port, const uint8_t handle[6],
                         const uint16_t *tx_ch, unsigned n);
uint16_t fcc_stop_flow(uint32_t ip, uint16_t port, const uint8_t handle[6]);
