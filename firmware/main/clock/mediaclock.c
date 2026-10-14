// SPDX-License-Identifier: GPL-3.0-or-later
//
// The device time base: the EMAC IEEE 1588 system time. Configured by
// net_start() for digital rollover (the sub-second register counts ns) and
// fine update (addend), disciplined here by the PTPv1 servo.
#include "clock/mediaclock.h"
#include "clock/mc_hw.h"
#include "clock/ptpv1.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/emac_ptp_struct.h"

#include "net/net_priv.h"

static const char *TAG = "mclock";

#define NS_PER_S            1000000000ull
#define HW_WAIT_LOOPS       2000        // self-clearing control bits clear in a few PTP clocks

static volatile bool    s_hw;           // counter configured and running
static volatile int32_t s_freq_ppb;
static _Atomic uint32_t s_epoch;
static _Atomic uint32_t s_step_seq;     // odd while mc_hw_step() is changing the counter
static uint32_t         s_addend_nominal;
static bool             s_started;
static portMUX_TYPE     s_hw_lock = portMUX_INITIALIZER_UNLOCKED;

// Before the 1588 counter runs (or without Ethernet), time comes from
// esp_timer. The +1 s keeps it non-zero: the EMAC driver reports an invalid
// timestamp as 0.
static inline uint64_t fallback_ns(void)
{
    return (uint64_t)esp_timer_get_time() * 1000ull + NS_PER_S;
}

// Seconds and nanoseconds are separate registers: re-read the nanoseconds if
// the seconds changed in between (at most one rollover per read).
static inline IRAM_ATTR uint64_t hw_read_ns(void)
{
    uint32_t s1 = EMAC_PTP.sys_seconds.val;
    uint32_t ns = EMAC_PTP.sys_nanosec.val & 0x7FFFFFFFu;
    uint32_t s2 = EMAC_PTP.sys_seconds.val;
    if (s1 != s2) {
        ns = EMAC_PTP.sys_nanosec.val & 0x7FFFFFFFu;
        s1 = s2;
    }
    return (uint64_t)s1 * NS_PER_S + ns;
}

static bool wait_ctrl_idle(void)
{
    for (int i = 0; i < HW_WAIT_LOOPS; i++) {
        uint32_t v = EMAC_PTP.timestamp_ctrl.val;
        // ts_initialize (bit 2), ts_update (bit 3), addend_reg_update (bit 5)
        if ((v & ((1u << 2) | (1u << 3) | (1u << 5))) == 0) {
            return true;
        }
    }
    return false;
}

// Load the counter with an absolute time (TSINIT). Caller holds s_hw_lock.
static bool hw_set_time(uint64_t t)
{
    if (!wait_ctrl_idle()) {
        return false;
    }
    EMAC_PTP.sys_seconds_update.val = (uint32_t)(t / NS_PER_S);
    EMAC_PTP.sys_nanosec_update.val = (uint32_t)(t % NS_PER_S);    // add_sub = 0
    EMAC_PTP.timestamp_ctrl.ts_initialize = 1;
    return wait_ctrl_idle();
}

void mc_hw_init(void)
{
    if (s_hw || !net_ptp_hw_ready()) {
        return;
    }
    s_addend_nominal = EMAC_PTP.timestamp_addend.val;
    portENTER_CRITICAL(&s_hw_lock);
    bool ok = hw_set_time(fallback_ns());
    portEXIT_CRITICAL(&s_hw_lock);
    if (!ok || s_addend_nominal == 0) {
        ESP_LOGE(TAG, "1588 counter did not accept init (addend %08lx); using esp_timer",
                 (unsigned long)s_addend_nominal);
        return;
    }
    s_hw = true;
    ESP_LOGI(TAG, "1588 counter running, nominal addend %08lx", (unsigned long)s_addend_nominal);
}

bool mc_hw_ready(void) { return s_hw; }

void mc_hw_step(int64_t delta_ns)
{
    if (!s_hw) {
        return;
    }
    portENTER_CRITICAL(&s_hw_lock);
    // seq odd: mc_now_ns_epoch() readers on the other core wait; the epoch
    // changes together with the timescale, never after it.
    atomic_fetch_add_explicit(&s_step_seq, 1, memory_order_acq_rel);
    int64_t t = (int64_t)hw_read_ns() + delta_ns;
    if (t < (int64_t)NS_PER_S) {
        t = (int64_t)NS_PER_S;          // keep timestamps non-zero
    }
    hw_set_time((uint64_t)t);
    atomic_fetch_add_explicit(&s_epoch, 1, memory_order_release);
    atomic_fetch_add_explicit(&s_step_seq, 1, memory_order_release);
    portEXIT_CRITICAL(&s_hw_lock);
}

void mc_hw_set_freq_ppb(double ppb)
{
    if (!s_hw) {
        return;
    }
    double a = (double)s_addend_nominal * (1.0 + ppb * 1e-9);
    if (a < 1.0) a = 1.0;
    if (a > 4294967295.0) a = 4294967295.0;
    uint32_t addend = (uint32_t)(a + 0.5);
    portENTER_CRITICAL(&s_hw_lock);
    if (wait_ctrl_idle()) {
        EMAC_PTP.timestamp_addend.val = addend;
        EMAC_PTP.timestamp_ctrl.addend_reg_update = 1;
        s_freq_ppb = (int32_t)(ppb >= 0 ? ppb + 0.5 : ppb - 0.5);
    }
    portEXIT_CRITICAL(&s_hw_lock);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void mc_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;
    mc_hw_init();
    ptpv1_start();
}

uint64_t IRAM_ATTR mc_now_ns(void)
{
    if (!s_hw) {
        return fallback_ns();
    }
    return hw_read_ns();
}

uint64_t IRAM_ATTR mc_now_ns_epoch(uint32_t *epoch)
{
    for (;;) {
        const uint32_t s1 = atomic_load_explicit(&s_step_seq, memory_order_acquire);
        if (s1 & 1u) {
            continue;                   // step in progress on the other core (µs)
        }
        const uint64_t t = mc_now_ns();
        const uint32_t e = atomic_load_explicit(&s_epoch, memory_order_acquire);
        if (atomic_load_explicit(&s_step_seq, memory_order_acquire) == s1) {
            *epoch = e;
            return t;
        }
    }
}

uint64_t IRAM_ATTR mc_ns_to_frames(uint64_t ns)
{
    // ns * 48000 / 1e9 == ns * 6 / 125000, split to avoid overflow
    return (ns / 125000u) * 6u + ((ns % 125000u) * 6u) / 125000u;
}

uint64_t mc_frames_to_ns(uint64_t f) { return (f / 6u) * 125000u + ((f % 6u) * 125000u) / 6u; }
uint64_t IRAM_ATTR mc_now_frames(void) { return mc_ns_to_frames(mc_now_ns()); }
bool mc_locked(void) { return ptpv1_locked(); }
uint32_t IRAM_ATTR mc_epoch(void) { return atomic_load_explicit(&s_epoch, memory_order_acquire); }
int32_t mc_freq_ppb(void) { return s_freq_ppb; }
int32_t mc_offset_ns(void) { return ptpv1_offset_ns(); }
void mc_leader_id(uint8_t id[8]) { ptpv1_leader_id(id); }
void mc_get_stats(mc_stats_t *s) { ptpv1_get_stats(s); }
