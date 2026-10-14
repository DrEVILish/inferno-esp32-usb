// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ethernet bring-up and the fast path that bypasses lwIP.
//
// - The EMAC RX path looks at every frame first. UDP datagrams to a port with
//   a registered hook are handed to the hook (in the EMAC RX task, core 0)
//   and never reach lwIP. Everything else goes to lwIP as normal.
// - Raw UDP TX builds Ethernet/IP/UDP headers itself and hands the frame
//   straight to the EMAC (audio flows). Control traffic uses lwIP sockets.
//
// Owner: net/ (net+clock agent).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

// Bring up EMAC + IP101, esp_netif (DHCP with link-local fallback), IGMP,
// enable IEEE 1588 timestamping. Returns once the driver runs; link/IP come
// later. Call before mc_start() and aoip_start().
esp_err_t net_start(void);

esp_netif_t *net_netif(void);
bool net_link_up(void);
uint32_t net_ipv4(void);                 // network byte order, 0 if none
void net_mac(uint8_t mac[6]);            // EMAC address (identity source)

// Receive hook. `payload` is the UDP payload; src_ip/dst_ip in network byte
// order, ports in host order. `rx_ts_ns` is the hardware RX timestamp (PTP
// time) when available, else 0 — the EMAC stamps only PTPv1 event messages
// (UDP 319), so audio hooks always see 0. Return true if consumed (the frame
// is then freed); false passes it on to lwIP.
typedef struct {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint64_t rx_ts_ns;
} net_udp_meta_t;
typedef bool (*net_udp_hook_t)(const uint8_t *payload, size_t len,
                               const net_udp_meta_t *meta, void *ctx);

// Register a hook for UDP destination ports [port_lo, port_hi]. Hooks run in
// the EMAC RX task on core 0 and must not block. Max 16 registrations.
esp_err_t net_udp_hook_register(uint16_t port_lo, uint16_t port_hi,
                                net_udp_hook_t hook, void *ctx);
esp_err_t net_udp_hook_unregister(uint16_t port_lo, uint16_t port_hi);

// Join/leave an IPv4 multicast group (IGMP via lwIP; the EMAC passes all
// multicast). Groups are re-joined automatically after a link loss. Groups in
// 239.0.0.0/8 (AoIP audio) MUST be joined here (not only via a socket's
// IP_ADD_MEMBERSHIP): the RX fast path drops 239/8 traffic for groups not in
// this table unless a hook claims its port. Not callable from a hook.
esp_err_t net_mcast_join(uint32_t group_ip);
esp_err_t net_mcast_leave(uint32_t group_ip);

// Raw UDP destination: resolved once (ARP via lwIP, or multicast MAC), then
// used for many sends. src_port is ours.
typedef struct {
    uint8_t  dst_mac[6];
    uint32_t dst_ip;            // network byte order
    uint16_t dst_port, src_port;
    uint8_t  dscp;              // 46 (EF) for audio, 56 (CS7) for PTP events
    bool     resolved;
} net_udp_dest_t;

// Resolve dst MAC (non-blocking; returns ESP_ERR_NOT_FINISHED while ARP is
// pending — call again later). Runs a short call in the lwIP thread, so it
// must not be called from a hook (EMAC RX task). Re-resolve now and then:
// ARP entries age out and peers can change address.
esp_err_t net_udp_dest_resolve(net_udp_dest_t *d);

// Send one UDP datagram on the raw path (no lwIP). Safe from the TX
// packetizer task. `len` <= 1472.
esp_err_t net_udp_send_raw(const net_udp_dest_t *d, const void *payload, size_t len);

// Counters for telemetry.
typedef struct {
    uint32_t rx_frames, rx_hooked, rx_to_lwip, rx_dropped;
    uint32_t tx_raw, tx_raw_fail;
    uint32_t rx_mcast_filtered;     // 239/8 multicast for groups nobody joined
} net_stats_t;
void net_get_stats(net_stats_t *s);
