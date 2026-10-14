// SPDX-License-Identifier: GPL-3.0-or-later
#include "sys/telem.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "freertos/idf_additions.h"

#include "app_config.h"
#include "audio/ring.h"
#include "clock/mediaclock.h"
#include "net/net.h"
#include "usb/uac2.h"
#include "aoip/aoip.h"
#include "sys/perf.h"

static const char *TAG = "telem";

// Build identity: the matrix name of this image (docs/21-latency-matrix.md).
#if CONFIG_USBINF_TEST_NET_LOOPBACK
#define BUILD_MODE "_NETLOOP"
#elif CONFIG_USBINF_TEST_USB_LOOPBACK
#define BUILD_MODE "_USBLOOP"
#elif CONFIG_USBINF_TEST_TONE
#define BUILD_MODE "_TONE"
#else
#define BUILD_MODE ""
#endif
#define STR2_(x) #x
#define STR_(x) STR2_(x)
#define BUILD_NAME "LAT" STR_(UI_LAT_PROFILE) "_CH" STR_(CONFIG_USBINF_CHANNELS) BUILD_MODE

// Windowed values print "none" when nothing was sampled since the last request.
#define KV_WIN(key, v, none) do { \
        if ((v) == (none)) KV(key "=none%s", ""); else KV(key "=%ld", (long)(v)); \
    } while (0)

#define KV(fmt, ...) do { \
        int _n = snprintf(p, (size_t)(end - p), fmt "\n", __VA_ARGS__); \
        if (_n > 0 && _n < end - p) p += _n; \
    } while (0)

static size_t snapshot(char *buf, size_t cap)
{
    char *p = buf, *end = buf + cap;
    uint8_t lid[8];
    net_stats_t ns;
    uac2_stats_t us;
    aoip_stats_t as;
    net_get_stats(&ns);
    uac2_get_stats(&us);
    aoip_get_stats(&as);
    mc_leader_id(lid);

    KV("uptime_ms=%llu", (unsigned long long)(esp_timer_get_time() / 1000));
    KV("fw=%s", UI_FW_VERSION);
    {
        char sha[17];
        esp_app_get_elf_sha256(sha, sizeof sha);
        KV("fw_build=%s", BUILD_NAME);
        KV("fw_elf_sha256=%s", sha);
    }
    KV("cfg_channels=%u", UI_CHANNELS);
    KV("cfg_lat_profile=%d", UI_LAT_PROFILE);
    KV("cfg_net_latency_frames=%u", UI_NET_LATENCY_FRAMES);
    KV("cfg_fpp=%u", UI_FPP_DEFAULT);
    KV("cfg_usb_out_lead_frames=%u", UI_USB_OUT_LEAD_FRAMES);
    KV("cfg_usb_in_margin_frames=%u", UI_USB_IN_MARGIN_FRAMES);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    {
        // idle share per core since the previous request (run-time counter = esp_timer µs)
        static uint64_t last_idle[2], last_t;
        const uint64_t t = (uint64_t)esp_timer_get_time();
        for (int c = 0; c < 2; c++) {
            const uint64_t idle = (uint64_t)ulTaskGetIdleRunTimeCounterForCore(c);
            const uint64_t dt = t - last_t;
            if (last_t && dt) KV("cpu%d_idle_pct=%u", c, (unsigned)((idle - last_idle[c]) * 100u / dt));
            last_idle[c] = idle;
        }
        last_t = t;
    }
#endif
    KV("heap_internal_free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    KV("heap_internal_min_free=%u", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    KV("heap_dma_largest=%u", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    // clock
    KV("ptp_locked=%d", mc_locked());
    KV("ptp_epoch=%lu", (unsigned long)mc_epoch());
    KV("ptp_offset_ns=%ld", (long)mc_offset_ns());
    KV("ptp_freq_ppb=%ld", (long)mc_freq_ppb());
    KV("ptp_leader=%02x%02x%02x%02x%02x%02x%02x%02x",
       lid[0], lid[1], lid[2], lid[3], lid[4], lid[5], lid[6], lid[7]);
    KV("media_frames=%llu", (unsigned long long)mc_now_frames());
    {
        mc_stats_t ms;
        mc_get_stats(&ms);
        KV("ptp_stage=%u", ms.stage);
        KV("ptp_path_delay_ns=%ld", (long)ms.path_delay_ns);
        KV("ptp_syncs=%lu", (unsigned long)ms.syncs);
        KV("ptp_follow_ups=%lu", (unsigned long)ms.follow_ups);
        KV("ptp_delay_reqs=%lu", (unsigned long)ms.delay_reqs);
        KV("ptp_delay_resps=%lu", (unsigned long)ms.delay_resps);
        KV("ptp_no_hw_rx_ts=%lu", (unsigned long)ms.no_hw_rx_ts);
        KV("ptp_no_hw_tx_ts=%lu", (unsigned long)ms.no_hw_tx_ts);
        KV("ptp_queue_drops=%lu", (unsigned long)ms.queue_drops);
        KV("ptp_steps=%lu", (unsigned long)ms.steps);
        KV("ptp_lock_losses=%lu", (unsigned long)ms.lock_losses);
        KV("ptp_outliers=%lu", (unsigned long)ms.outliers);
        KV("ptp_phase_shifts=%lu", (unsigned long)ms.phase_shifts);
        KV("ptp_leader_changes=%lu", (unsigned long)ms.leader_changes);
        KV("ptp_delay_samples=%lu", (unsigned long)ms.delay_samples);
        KV("ptp_delay_rejected=%lu", (unsigned long)ms.delay_rejected);
        KV("ptp_hw_step_fails=%lu", (unsigned long)ms.hw_step_fails);
        KV("ptp_hw_freq_fails=%lu", (unsigned long)ms.hw_freq_fails);
    }
    // net
    KV("net_link=%d", net_link_up());
    KV("net_rx_frames=%lu", (unsigned long)ns.rx_frames);
    KV("net_rx_hooked=%lu", (unsigned long)ns.rx_hooked);
    KV("net_rx_to_lwip=%lu", (unsigned long)ns.rx_to_lwip);
    KV("net_rx_dropped=%lu", (unsigned long)ns.rx_dropped);
    KV("net_tx_raw=%lu", (unsigned long)ns.tx_raw);
    KV("net_tx_raw_fail=%lu", (unsigned long)ns.tx_raw_fail);
    KV("net_rx_mcast_filtered=%lu", (unsigned long)ns.rx_mcast_filtered);
    KV("net_ipv4=%u.%u.%u.%u", (unsigned)(net_ipv4() & 0xFF), (unsigned)((net_ipv4() >> 8) & 0xFF),
       (unsigned)((net_ipv4() >> 16) & 0xFF), (unsigned)(net_ipv4() >> 24));
    // usb
    KV("usb_sof=%lu", (unsigned long)us.sof_count);
    KV("usb_out_streaming=%u", us.out_streaming);
    KV("usb_in_streaming=%u", us.in_streaming);
    KV("usb_out_frames=%lu", (unsigned long)us.out_frames);
    KV("usb_in_frames=%lu", (unsigned long)us.in_frames);
    KV("usb_out_overruns=%lu", (unsigned long)us.out_overruns);
    KV("usb_out_underruns=%lu", (unsigned long)us.out_underruns);
    KV("usb_in_underruns=%lu", (unsigned long)us.in_underruns);
    KV("usb_feedback_q16=%lu", (unsigned long)us.feedback_q16);
    KV("usb_out_lead_frames=%ld", (long)us.out_lead_frames);
    KV("usb_sof_missed=%lu", (unsigned long)us.sof_missed);
    KV("usb_sof_late=%lu", (unsigned long)us.sof_late);
    KV("usb_sof_gaps=%lu", (unsigned long)us.sof_gaps);
    KV("usb_sof_fn_bad=%lu", (unsigned long)us.sof_fn_bad);
    KV("usb_epoch_changes=%lu", (unsigned long)us.epoch_changes);
    KV("usb_out_packets=%lu", (unsigned long)us.out_packets);
    KV("usb_in_packets=%lu", (unsigned long)us.in_packets);
    KV("usb_out_gap_fills=%lu", (unsigned long)us.out_gap_fills);
    KV("usb_out_gap_frames=%lu", (unsigned long)us.out_gap_frames);
    KV("usb_out_bad_len=%lu", (unsigned long)us.out_bad_len);
    KV("usb_in_catchup=%lu", (unsigned long)us.in_catchup);
    KV("usb_in_resyncs=%lu", (unsigned long)us.in_resyncs);
    KV("usb_xfer_errors=%lu", (unsigned long)us.xfer_errors);
    KV("usb_arm_failures=%lu", (unsigned long)us.arm_failures);
    KV("usb_ep_errors=%lu", (unsigned long)us.ep_errors);
    KV("usb_fb_phase_ppb=%ld", (long)us.fb_phase_ppb);
    KV("usb_configured=%u", us.configured);
    KV("usb_high_speed=%u", us.high_speed);
    KV("usb_out_alt=%u", us.out_alt);
    KV("usb_in_alt=%u", us.in_alt);
    // latency windows (frames, since the previous request)
    KV_WIN("lat_usb_out_headroom_min", us.out_hr_min, INT32_MAX);
    KV_WIN("lat_usb_out_headroom_max", us.out_hr_max, INT32_MIN);
    KV_WIN("lat_usb_in_age_min", us.in_age_min, INT32_MAX);
    KV_WIN("lat_usb_in_age_max", us.in_age_max, INT32_MIN);
    // aoip
    KV("aoip_tx_flows=%lu", (unsigned long)as.tx_flows);
    KV("aoip_rx_flows=%lu", (unsigned long)as.rx_flows);
    KV("aoip_tx_packets=%lu", (unsigned long)as.tx_packets);
    KV("aoip_rx_packets=%lu", (unsigned long)as.rx_packets);
    KV("aoip_rx_late=%lu", (unsigned long)as.rx_late);
    KV("aoip_rx_early=%lu", (unsigned long)as.rx_early);
    KV("aoip_rx_seq_gaps=%lu", (unsigned long)as.rx_seq_gaps);
    KV("aoip_subs_total=%lu", (unsigned long)as.subscriptions_total);
    KV("aoip_subs_ok=%lu", (unsigned long)as.subscriptions_ok);
    KV("aoip_tx_send_fail=%lu", (unsigned long)as.tx_send_fail);
    KV("aoip_tx_underruns=%lu", (unsigned long)as.tx_underruns);
    KV("aoip_tx_lag_resets=%lu", (unsigned long)as.tx_lag_resets);
    KV("aoip_rx_wrong_len=%lu", (unsigned long)as.rx_wrong_len);
    KV("aoip_rx_ka_sent=%lu", (unsigned long)as.rx_ka_sent);
    KV("aoip_rx_ka_fail=%lu", (unsigned long)as.rx_ka_fail);
    KV("aoip_fc_requests=%lu", (unsigned long)as.fc_requests);
    KV("aoip_fc_failures=%lu", (unsigned long)as.fc_failures);
    KV("aoip_arc_requests=%lu", (unsigned long)as.arc_requests);
    KV("aoip_nvs_writes=%lu", (unsigned long)as.nvs_writes);
    KV("aoip_nvs_skipped=%lu", (unsigned long)as.nvs_skipped);
    KV("aoip_info_drops=%lu", (unsigned long)as.info_drops);
    KV_WIN("lat_rx_margin_min", as.rx_margin_min, INT32_MAX);
    for (unsigned i = 0; i < UI_RX_MAX_FLOWS; i++) {
        if (as.rx_slot_margin_min[i] != INT32_MAX) {
            KV("lat_rx_slot%u_margin_min=%ld", i, (long)as.rx_slot_margin_min[i]);
        }
    }
    KV_WIN("lat_tx_send_lag_max", as.tx_lag_max, INT32_MIN);
    // rings
    KV("ring_n2u_underruns=%lu", (unsigned long)atomic_load(&g_ring_net_to_usb.underruns));
    KV("ring_n2u_late=%lu", (unsigned long)atomic_load(&g_ring_net_to_usb.late));
    KV("ring_u2n_underruns=%lu", (unsigned long)atomic_load(&g_ring_usb_to_net.underruns));
    // perf probes (docs/15): windowed cycle histograms since the previous request
    KV("net_rx_dma_free=%u", ns.rx_dma_free_desc);
    for (unsigned pi = 0; pi < PERF_N; pi++) {
        perf_window_t w;
        perf_report((perf_probe_t)pi, &w);
        if (!w.n) continue;
        KV("t_%s_n=%lu", perf_name((perf_probe_t)pi), (unsigned long)w.n);
        KV("t_%s_p50=%lu", perf_name((perf_probe_t)pi), (unsigned long)w.p50);
        KV("t_%s_p99=%lu", perf_name((perf_probe_t)pi), (unsigned long)w.p99);
        KV("t_%s_max=%lu", perf_name((perf_probe_t)pi), (unsigned long)w.max);
        KV("t_%s_ovf=%lu", perf_name((perf_probe_t)pi), (unsigned long)w.overflow);
    }
    return (size_t)(p - buf);
}

static void telem_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(UI_TELEM_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGE(TAG, "socket/bind failed");
        vTaskDelete(NULL);
        return;
    }
    static char buf[8192];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        char req[64];
        if (recvfrom(s, req, sizeof req, 0, (struct sockaddr *)&from, &fl) < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t n = snapshot(buf, sizeof buf);
        sendto(s, buf, n, 0, (struct sockaddr *)&from, fl);
    }
}

esp_err_t telem_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(telem_task, "telem", 4096, NULL,
                                            UI_PRIO_CONTROL - 2, NULL, UI_CORE_USB);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
