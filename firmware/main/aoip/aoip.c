// SPDX-License-Identifier: GPL-3.0-or-later
//
// AoIP device: identity, start-up and the control task.
//
// The control task serves ARC (4440), CMC (8800), the info request port
// (8700) and the flow-control server (4455) from one select() loop, sends
// the 1 Hz heartbeat, ages TX flows, sends RX keepalives, flushes NVS and
// watches the IP address (re-announcing on change). It is the Inferno
// device_server task set (arc_server.rs, cmc_server.rs, info_mcast_server.rs,
// flows_control_server.rs) collapsed into one thread.
#include "aoip/aoip.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "app_config.h"
#include "net/net.h"
#include "aoip/aoip_internal.h"
#include "aoip/proto.h"

static const char *TAG = "aoip";

aoip_dev_t g_aoip;
aoip_counters_t g_aoip_ctr;

static SemaphoreHandle_t s_lock;

void aoip_lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void aoip_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

uint32_t aoip_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void aoip_factory_ch_name(char out[4], unsigned index)
{
    unsigned id = index + 1u;
    out[0] = (char)('0' + (id / 10u) % 10u);
    out[1] = (char)('0' + id % 10u);
    out[2] = 0;
}

static void identity_init(void)
{
    net_mac(g_aoip.mac);
    const uint8_t *m = g_aoip.mac;
    // EUI-64 of the MAC. The cmc "id=" TXT, the CMC advertisement and the
    // multicast headers must all carry this same id.
    const uint8_t devid[8] = { m[0], m[1], m[2], 0xff, 0xfe, m[3], m[4], m[5] };
    memcpy(g_aoip.devid, devid, 8);
    g_aoip.process_id = 0;
    snprintf(g_aoip.default_hostname, sizeof g_aoip.default_hostname, "%s%02x%02x%02x",
             UI_NAME_PREFIX, m[3], m[4], m[5]);
    strcpy(g_aoip.friendly_hostname, g_aoip.default_hostname);
    // settings.rs: format!("{short_app_name}-{}", hex::encode(devid))
    snprintf(g_aoip.factory_hostname, sizeof g_aoip.factory_hostname,
             "%s-%02x%02x%02x%02x%02x%02x%02x%02x", UI_MODEL_ID,
             devid[0], devid[1], devid[2], devid[3], devid[4], devid[5], devid[6], devid[7]);
    for (unsigned i = 0; i < UI_CHANNELS; i++) {
        aoip_factory_ch_name(g_aoip.tx_names[i], i);
        aoip_factory_ch_name(g_aoip.rx_names[i], i);
    }
    atomic_store(&g_aoip.ip, 0);
}

static int udp_bind(uint16_t port)
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
        ESP_LOGE(TAG, "bind %u failed", port);
        close(s);
        return -1;
    }
    return s;
}

static void check_ip(void)
{
    uint32_t ip = net_ipv4();
    if (ip == atomic_load(&g_aoip.ip)) return;

    esp_netif_ip_info_t info = { 0 };
    esp_netif_t *nif = net_netif();
    if (nif) esp_netif_get_ip_info(nif, &info);
    aoip_lock();
    atomic_store(&g_aoip.ip, ip);
    g_aoip.netmask = info.netmask.addr;
    g_aoip.gw = info.gw.addr;
    aoip_unlock();

    if (ip) {
        ESP_LOGI(TAG, "IP " IPSTR ", announcing as %s", IP2STR((esp_ip4_addr_t *)&ip),
                 g_aoip.friendly_hostname);
        aoip_mdns_ip_changed();
        info_send_board_and_product();
    } else {
        ESP_LOGW(TAG, "IP lost");
    }
}

typedef size_t (*rr_handler_t)(const uint8_t *, size_t, uint8_t *, size_t);

static void serve(int s, rr_handler_t h, uint8_t *rx, uint8_t *tx)
{
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = recvfrom(s, rx, 1500, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n < (int)RR_HEADER_LEN) return;
    aoip_lock();
    size_t len = h(rx, (size_t)n, tx, 1500);
    aoip_unlock();
    if (len) sendto(s, tx, len, 0, (struct sockaddr *)&from, fl);
}

static void serve_info(int s, uint8_t *rx)
{
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = recvfrom(s, rx, 1500, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n <= 0) return;
    aoip_lock();
    info_on_request(rx, (size_t)n);
    aoip_unlock();
}

static void ctl_task(void *arg)
{
    static uint8_t rx[1500], tx[1500];

    if (aoip_mdns_init() != ESP_OK) ESP_LOGE(TAG, "mDNS init failed");
    aoip_lock();
    aoip_mdns_publish_all();
    txm_load_state();
    aoip_unlock();

    int s_arc = udp_bind(AOIP_PORT_ARC);
    int s_cmc = udp_bind(AOIP_PORT_CMC);
    int s_info = udp_bind(AOIP_PORT_INFO_REQUEST);
    int s_fcs = udp_bind(AOIP_PORT_FLOWS_CONTROL);
    if (s_info >= 0) {
        uint8_t ttl = 1;
        setsockopt(s_info, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    }
    info_set_socket(s_info);        // creates the RX-channel notify queue ...
    // ... which the subscriber task posts to: start it only now
    if (sub_start() != ESP_OK) ESP_LOGE(TAG, "subscriber start failed");

    uint32_t t_ip = 0, t_hb = aoip_now_ms(), t_100 = 0, t_ka = 0;
    for (;;) {
        uint32_t now = aoip_now_ms();
        if (now - t_ip >= 200u) {
            t_ip = now;
            check_ip();
        }

        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = -1;
        const int socks[4] = { s_arc, s_cmc, s_info, s_fcs };
        for (int i = 0; i < 4; i++) {
            if (socks[i] >= 0) {
                FD_SET(socks[i], &rf);
                if (socks[i] > maxfd) maxfd = socks[i];
            }
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
        int n = maxfd >= 0 ? select(maxfd + 1, &rf, NULL, NULL, &tv) : 0;
        if (maxfd < 0) vTaskDelay(pdMS_TO_TICKS(10));
        if (n > 0) {
            if (s_arc >= 0 && FD_ISSET(s_arc, &rf)) {
                CTR_INC(arc_requests);
                serve(s_arc, arc_handle, rx, tx);
            }
            if (s_cmc >= 0 && FD_ISSET(s_cmc, &rf)) serve(s_cmc, cmc_handle, rx, tx);
            if (s_fcs >= 0 && FD_ISSET(s_fcs, &rf)) serve(s_fcs, fcs_handle, rx, tx);
            if (s_info >= 0 && FD_ISSET(s_info, &rf)) serve_info(s_info, rx);
        }

        aoip_lock();
        info_poll();
        now = aoip_now_ms();
        if (now - t_hb >= 1000u) {
            // tokio interval with MissedTickBehavior::Delay
            t_hb = (now - t_hb >= 2000u) ? now : t_hb + 1000u;
            info_heartbeat();
        }
        if (now - t_100 >= 100u) {
            t_100 = now;
            ftx_tick();
            txm_tick();
        }
        if (now - t_ka >= MEDIA_KEEPALIVE_INTERVAL_MS) {
            t_ka = now;
            frx_keepalive_tick();
        }
        aoip_unlock();
        // Both are self-locking and slow (mDNS round-trips, NVS commit):
        // outside the shared hold so control-plane requests keep flowing.
        aoip_mdns_publish_if_pending();
        settings_flush_if_due();
    }
}

esp_err_t aoip_start(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    identity_init();
    settings_load();
    ESP_LOGI(TAG, "device %s (factory %s), %u TX x %u RX, %u Hz/%u-bit, latency %u us",
             g_aoip.friendly_hostname, g_aoip.factory_hostname, UI_CHANNELS, UI_CHANNELS,
             UI_SAMPLE_RATE, UI_NET_BITS, (unsigned)(UI_NET_LATENCY_NS / 1000u));

    esp_err_t err = frx_init();
    if (err != ESP_OK) return err;
    err = ftx_init();
    if (err != ESP_OK) return err;

    BaseType_t ok = xTaskCreatePinnedToCore(ctl_task, "aoip_ctl", 8192, NULL, UI_PRIO_CONTROL,
                                            NULL, UI_CORE_USB);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void aoip_get_stats(aoip_stats_t *s)
{
    memset(s, 0, sizeof *s);
#define LD(f) atomic_load_explicit(&g_aoip_ctr.f, memory_order_relaxed)
    s->tx_packets = LD(tx_packets);
    s->rx_packets = LD(rx_packets);
    s->rx_late = LD(rx_late);
    s->rx_early = LD(rx_early);
    s->rx_seq_gaps = LD(rx_seq_gaps);
    s->tx_send_fail = LD(tx_send_fail);
    s->tx_underruns = LD(tx_underruns);
    s->tx_lag_resets = LD(tx_lag_resets);
    s->rx_wrong_len = LD(rx_wrong_len);
    s->rx_ka_sent = LD(rx_ka_sent);
    s->rx_ka_fail = LD(rx_ka_fail);
    s->fc_requests = LD(fc_requests);
    s->fc_failures = LD(fc_failures);
    s->arc_requests = LD(arc_requests);
    s->nvs_writes = LD(nvs_writes);
    s->info_drops = LD(info_drops);
#undef LD
    s->nvs_skipped = settings_skipped_flushes();
    s->rx_margin_min = INT32_MAX;
    for (unsigned i = 0; i < UI_RX_MAX_FLOWS; i++) {
        s->rx_slot_margin_min[i] = frx_take_min_margin(i);
        if (s->rx_slot_margin_min[i] < s->rx_margin_min) s->rx_margin_min = s->rx_slot_margin_min[i];
    }
    s->tx_lag_max = ftx_take_lag_max();
    if (!s_lock) return;
    aoip_lock();
    s->tx_flows = ftx_active_count();
    s->rx_flows = frx_active_count();
    sub_counts(&s->subscriptions_total, &s->subscriptions_ok);
    aoip_unlock();
}
