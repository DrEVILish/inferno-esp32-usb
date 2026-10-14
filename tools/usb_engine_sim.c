// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-side simulation of the USB clock/position engine
// (firmware/main/usb/uac2_engine.c). No hardware needed.
//
// Models a USB host whose microframe clock is off by host_ppm, a media
// clock (PTP) off by media_ppm, SOF interrupt latency jitter, OUT packet
// sizes chosen by the host from the feedback value (16.16 accumulator, as
// Linux/macOS/Windows do), optional lost OUT packets and a media clock step.
// In the loopback build it also checks that capture returns playback
// bit-exact after a constant delay.
//
// Build + run (both variants):   tools/usb_engine_sim.sh
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/ring.h"
#include "clock/mediaclock.h"
#include "usb/uac2_engine.h"

// ---------------------------------------------------------------------------
// Stand-ins for ring.c and mediaclock.c
// ---------------------------------------------------------------------------
audio_ring_t g_ring_net_to_usb;
audio_ring_t g_ring_usb_to_net;
#define FB (UI_CHANNELS * sizeof(int32_t))

void ring_init(void)
{
    g_ring_net_to_usb.buf = calloc(RING_FRAMES, FB);
    g_ring_usb_to_net.buf = calloc(RING_FRAMES, FB);
}
void ring_read_frames(audio_ring_t *r, uint64_t f, int32_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++, dst += UI_CHANNELS) memcpy(dst, ring_frame(r, f + i), FB);
}

void ring_clear_frames(audio_ring_t *r, uint64_t f, size_t n)
{
    for (size_t i = 0; i < n; i++) memset(ring_frame(r, f + i), 0, FB);
}

void ring_read_frames_and_clear(audio_ring_t *r, uint64_t f, int32_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++, f++, dst += UI_CHANNELS) {
        memcpy(dst, ring_frame(r, f), FB);
        memset(ring_frame(r, f), 0, FB);
    }
}
void ring_write_frames(audio_ring_t *r, uint64_t f, const int32_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++, src += UI_CHANNELS) memcpy(ring_frame(r, f + i), src, FB);
    atomic_store(&r->head, f + n);
}
void ring_write_silence(audio_ring_t *r, uint64_t f, size_t n)
{
    for (size_t i = 0; i < n; i++) memset(ring_frame(r, f + i), 0, FB);
    atomic_store(&r->head, f + n);
}
uint64_t mc_ns_to_frames(uint64_t ns)
{
    return (ns / 125000u) * 6u + ((ns % 125000u) * 6u) / 125000u;
}

// ---------------------------------------------------------------------------
static uint32_t rng = 12345;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

static int32_t pattern(uint64_t h, unsigned c)
{
    uint32_t v = (uint32_t)(h * 2654435761u) ^ (c * 40503u) ^ (uint32_t)(h >> 7);
    v &= 0xFFFFFFu;
    if (v == 0) v = 1;                              // never zero: silence is detectable
    return (int32_t)(v << 8);
}

typedef struct {
    const char *name;
    double host_ppm, media_ppm;
    double seconds;
    double drop_every_s;        // 0 = never
    double step_at_s;           // 0 = never; media clock steps +10 ms
    double blackout_every_s;    // 0 = never; USB ISR blocked for 1 ms (8 uframes)
    bool fn_is_frame;           // DSTS reports 1 ms frame numbers instead of microframes
    bool late_out;              // OUT completion anywhere in the microframe (0..125 us) + 60 us spikes
} scen_t;

// TX-side headroom: (first frame of the host data - real now) at each OUT
// write, i.e. after any silence the engine inserted for lost packets (those
// frames are lost either way). The AoIP packetizer reads frame f no earlier
// than f + 1 (the last frame of a packet is due when the packet is), so a
// write is in time iff that headroom >= 0. Counted once streaming is
// established, away from a media clock step.
static int64_t s_hr_min;
static uint64_t s_hr_late, s_hr_n;
static void headroom(const uac2_engine_t *e, uint64_t now, uint32_t nbytes, bool count)
{
    if (!count || !eng_out_active(e) || nbytes < UAC2_FRAME_BYTES) return;
    int64_t hr = (int64_t)(e->w - nbytes / UAC2_FRAME_BYTES - now);
    if (hr < s_hr_min) s_hr_min = hr;
    if (hr < 0) s_hr_late++;
    s_hr_n++;
}

static int run(const scen_t *s)
{
    static uac2_engine_t e;
    eng_init(&e);
    s_hr_min = INT64_MAX; s_hr_late = 0; s_hr_n = 0;
    memset(g_ring_usb_to_net.buf, 0, RING_FRAMES * FB);
    memset(g_ring_net_to_usb.buf, 0, RING_FRAMES * FB);
    atomic_store(&g_ring_usb_to_net.head, 0);

    const double uf_ns = 125000.0 * (1.0 + s->host_ppm * 1e-6);
    const double media_k = 1.0 + s->media_ppm * 1e-6;
    double media_off = 5e9;             // media clock already 5 s in
    uint32_t epoch = 0;
    const uint64_t n_uf = (uint64_t)(s->seconds * 8000.0);
    const uint64_t start_out = 8000, start_in = 8000;   // streams start after 1 s

    uint64_t host_acc = 0, host_frame = 0;
    uint32_t fb_seen = 6u << 16, fb_next = 6u << 16;
    uint64_t next_drop = s->drop_every_s > 0 ? (uint64_t)(s->drop_every_s * 8000) : UINT64_MAX;
    bool stepped = false;

    static int32_t out_pkt[UAC2_MAX_FPU * UI_CHANNELS];
    static int32_t pend_pkt[UAC2_MAX_FPU * UI_CHANNELS];
    bool armed = false, pending = false;
    uint64_t arm_k = 0, arm_uf = 0, pend_arm_uf = 0;
    uint32_t pend_bytes = 0;
    static int32_t in_pkt[UAC2_MAX_FPU * UI_CHANNELS];
    unsigned hist_in[16] = {0};
    uint32_t catchup0 = 0;
#ifdef CONFIG_USBINF_TEST_USB_LOOPBACK
    // capture verification
    int64_t delay = 0;                   // host frame index - capture index
    bool have_delay = false;
    uint64_t cap_index = 0, cap_ok = 0, cap_bad = 0, cap_silence = 0, delay_changes = 0;
#endif
    double lead_sum = 0; uint64_t lead_n = 0;
    int32_t ppb_min = 1 << 30, ppb_max = -(1 << 30);

    // map host frame -> pattern; capture compares against it
    for (uint64_t k = 0; k < n_uf; k++) {
        double t = k * uf_ns;
        if (s->step_at_s > 0 && !stepped && t > s->step_at_s * 1e9) {
            media_off += 10e6;
            epoch++;
            stepped = true;
        }
        // ISR blackout: SOFs, OUT packets and IN slots of 8 microframes are
        // lost; the SOF interrupt that finally runs reports the latest frame.
        bool dark = false;
        if (s->blackout_every_s > 0) {
            uint64_t per = (uint64_t)(s->blackout_every_s * 8000);
            uint64_t ph = k % per;
            dark = (k > per) && ph < 8;
        }
        double lat = 1000 + urand() * 14000 + (urand() < 0.001 ? 60000 : 0);
        double m_sof = (t + lat) * media_k + media_off;
        uint32_t fn = s->fn_is_frame ? (uint32_t)((k >> 3) & 0x7FF) : (uint32_t)(k & 0x3FFF);
        if (!dark) eng_sof(&e, (uint64_t)m_sof, epoch, fn);

        if (k == start_out) eng_out_alt(&e, true);
        if (k == start_in) eng_in_alt(&e, true);

        // feedback EP: host polls every 8 uframes, value armed 1 ms earlier
        if (k >= start_out && (k % 8) == 0) {
            fb_seen = fb_next;
            fb_next = eng_fb_q16(&e);
        }

        // OUT: DWC2 iso OUT semantics as modelled here: a transfer armed in
        // microframe A accepts only a packet of matching parity (A+1, A+3..);
        // others are dropped. Completion runs in the ISR (late during a
        // blackout), which re-arms first and then hands the data over.
        const bool count_hr = k > start_out + 800 &&
            !(s->step_at_s > 0 && fabs(t * 1e-9 - s->step_at_s) < 0.2);
        if (!dark && pending) {
            uint64_t now_p = mc_ns_to_frames((uint64_t)((t + lat + 2000) * media_k + media_off));
            eng_out_packet(&e, now_p, pend_arm_uf, pend_pkt, pend_bytes);
            headroom(&e, now_p, pend_bytes, count_hr);
            pending = false;
            armed = true; arm_k = k; arm_uf = eng_uf(&e);
        }
        if (k == start_out) {
            armed = true; arm_k = k; arm_uf = eng_uf(&e);      // first arm at this SOF
        }
        if (k >= start_out) {
            host_acc += fb_seen;
            uint32_t n = (uint32_t)(host_acc >> 16);
            host_acc &= 0xFFFF;
            for (uint32_t i = 0; i < n; i++)
                for (unsigned c = 0; c < UI_CHANNELS; c++)
                    out_pkt[i * UI_CHANNELS + c] = pattern(host_frame + i, c);
            host_frame += n;
            bool drop = (k >= next_drop);
            if (drop) next_drop += (uint64_t)(s->drop_every_s * 8000);
            bool accept = !drop && armed && ((k ^ (arm_k + 1)) & 1) == 0 && k > arm_k;
            if (accept && dark) {
                memcpy(pend_pkt, out_pkt, n * UAC2_FRAME_BYTES);
                pend_bytes = n * UAC2_FRAME_BYTES;
                pend_arm_uf = arm_uf;
                pending = true;
                armed = false;
            } else if (accept) {
                double t_arr = s->late_out
                    ? t + urand() * 125000 + 2000 + urand() * 8000 + (urand() < 0.01 ? 60000 : 0)
                    : t + 15000 + urand() * 40000;
                uint64_t now = mc_ns_to_frames((uint64_t)(t_arr * media_k + media_off));
                uint64_t a_uf = arm_uf;
                arm_k = k; arm_uf = eng_uf(&e);                 // re-arm, then process
                eng_out_packet(&e, now, a_uf, out_pkt, n * UAC2_FRAME_BYTES);
                headroom(&e, now, n * UAC2_FRAME_BYTES, count_hr);
            }
        }

        if (k >= start_in && !dark) {
            uint32_t bytes = eng_in_packet(&e, in_pkt);
            uint32_t n = bytes / UAC2_FRAME_BYTES;
            if (k > start_in + 16 && e.st.in_catchup == catchup0) hist_in[n < 16 ? n : 15]++;
            catchup0 = e.st.in_catchup;
#ifdef CONFIG_USBINF_TEST_USB_LOOPBACK
            for (uint32_t i = 0; i < n; i++, cap_index++) {
                int32_t *f = &in_pkt[i * UI_CHANNELS];
                if (f[0] == 0) { cap_silence++; continue; }
                // find host frame: expect cap_index + delay
                bool ok = false;
                if (have_delay) {
                    uint64_t h = cap_index + (uint64_t)delay;
                    ok = true;
                    for (unsigned c = 0; c < UI_CHANNELS; c++)
                        if (f[c] != pattern(h, c)) { ok = false; break; }
                }
                if (!ok) {
                    // re-acquire: search a window of host frames
                    int64_t found = -1;
                    for (int64_t h = (int64_t)host_frame - 4000; h < (int64_t)host_frame; h++) {
                        if (h < 0) continue;
                        bool m = true;
                        for (unsigned c = 0; c < UI_CHANNELS; c++)
                            if (f[c] != pattern((uint64_t)h, c)) { m = false; break; }
                        if (m) { found = h; break; }
                    }
                    if (found >= 0) {
                        if (have_delay) delay_changes++;
                        have_delay = true;
                        delay = found - (int64_t)cap_index;
                        ok = true;
                    }
                }
                if (ok) cap_ok++; else cap_bad++;
            }
#endif
        }

        if (k > n_uf / 2 && eng_out_active(&e) && (k % 64) == 0) {
            lead_sum += eng_out_lead(&e);
            lead_n++;
            if (e.phase_ppb < ppb_min) ppb_min = e.phase_ppb;
            if (e.phase_ppb > ppb_max) ppb_max = e.phase_ppb;
        }
    }

    double expect = 6.0 * media_k * (1.0 + s->host_ppm * 1e-6);
    double meas = e.fb_raw_q24 / 16777216.0;
    double err_ppm = (meas / expect - 1.0) * 1e6;
    double lead_avg = lead_n ? lead_sum / lead_n : 0;

    printf("== %s\n", s->name);
    printf("  fb_raw %.7f expected %.7f (%.2f ppm)  phase_ppb [%d..%d] (2nd half)\n",
           meas, expect, err_ppm, ppb_min, ppb_max);
    printf("  out: lead avg %.2f (target %d) pkts %u frames %u under %u over %u gapfills %u (%u fr) zlp %u\n",
           lead_avg, ENG_OUT_LEAD, e.st.out_packets, e.st.out_frames, e.st.out_underruns,
           e.st.out_overruns, e.st.out_gap_fills, e.st.out_gap_frames, e.st.out_zlp);
    unsigned in_total = 0;
    for (int i = 0; i < 16; i++) in_total += hist_in[i];
    unsigned in_other = in_total - (hist_in[5] + hist_in[6] + hist_in[7]);
    printf("  in : pkts %u frames %u sizes 5/6/7/other: %u/%u/%u/%u under %u catchup %u resyncs %u\n",
           e.st.in_packets, e.st.in_frames, hist_in[5], hist_in[6], hist_in[7],
           in_other,
           e.st.in_underruns, e.st.in_catchup, e.st.in_resyncs);
    printf("  tx headroom (w - now at write): min %lld frames, late writes %llu of %llu\n",
           (long long)(s_hr_n ? s_hr_min : 0), (unsigned long long)s_hr_late,
           (unsigned long long)s_hr_n);
    printf("  sof: %u missed %u gaps %u epochs %u late %u refits %u fn_bad %u\n", e.st.sof_count,
           e.st.sof_missed, e.st.sof_gaps, e.st.epoch_changes, e.st.sof_late, e.st.sof_refits,
           e.st.sof_fn_bad);
#ifdef CONFIG_USBINF_TEST_USB_LOOPBACK
    printf("  loopback: ok %llu bad %llu silence %llu delay %lld frames, delay changes %llu\n",
           (unsigned long long)cap_ok, (unsigned long long)cap_bad,
           (unsigned long long)cap_silence, (long long)delay, (unsigned long long)delay_changes);
#endif

    int fail = 0;
    if (fabs(err_ppm) > 3.0) { printf("  FAIL: feedback rate error\n"); fail = 1; }
    if (fabs(lead_avg - ENG_OUT_LEAD) > 3.0) { printf("  FAIL: lead not held\n"); fail = 1; }
    if (in_other > (s->step_at_s > 0 ? 8u : 0u)) { printf("  FAIL: IN sizes outside 5..7\n"); fail = 1; }
    // A 1 ms ISR blackout (a flash write before A2, or a long critical
    // section) is only absorbed by a lead of at least 1 ms; with a shorter
    // lead it is reported, not failed: the low-latency profiles rely on the
    // USB ISR never being held off that long (docs/14-latency.md).
    const bool tolerate = s->blackout_every_s > 0 && ENG_OUT_LEAD < 60;
    if ((e.st.out_underruns || e.st.out_overruns) && !tolerate) {
        printf("  FAIL: OUT re-anchored\n"); fail = 1;
    }
    if (s_hr_late && !tolerate) {
        printf("  FAIL: OUT frames written after the packetizer needed them\n"); fail = 1;
    }
    if (tolerate && (e.st.out_underruns || s_hr_late))
        printf("  (expected with lead %d < 60: a 1 ms stall needs lead >= 1 ms + 1 uframe)\n", ENG_OUT_LEAD);
#ifdef CONFIG_USBINF_TEST_USB_LOOPBACK
    if (cap_bad) { printf("  FAIL: loopback mismatch\n"); fail = 1; }
    if (s->step_at_s == 0 && delay_changes) { printf("  FAIL: loopback delay changed\n"); fail = 1; }
#endif
    printf("  %s\n", fail ? "FAILED" : "ok");
    return fail;
}

int main(void)
{
    ring_init();
    printf("UI_CHANNELS %u, latency profile %d: USB OUT lead %d frames, IN margin %d frames\n",
           UI_CHANNELS, UI_LAT_PROFILE, ENG_OUT_LEAD, ENG_IN_MARGIN);
    const scen_t sc[] = {
        // name                          host  media  secs drop  step  blackout fn_is_frame late_out
        { "nominal clocks",                 0,    0,   30, 0,   0,  0,   false, false },
        { "host -30 ppm, media +80 ppm",  -30,   80,   60, 0,   0,  0,   false, false },
        { "host +100 ppm, media -100 ppm", 100, -100,  60, 0,   0,  0,   false, false },
        { "lost OUT packet every 7 s",     20,   -5,   40, 7.0, 0,  0,   false, false },
        { "media clock step at 20 s",      10,   10,   40, 0,   20, 0,   false, false },
        { "1 ms ISR blackout every 3 s",   40,  -20,   40, 0,   0,  3.0, false, false },
        { "DSTS gives frame numbers",     -15,   25,   30, 0,   0,  0,   true,  false },
        { "OUT completes late in uframe",  25,  -40,   60, 0,   0,  0,   false, true },
        { "late OUT + lost every 3 s",    -60,   60,   60, 3.0, 0,  0,   false, true },
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof sc / sizeof sc[0]; i++) fails += run(&sc[i]);
    printf("%s (%d failed)\n", fails ? "SOME FAILED" : "ALL OK", fails);
    return fails ? 1 : 0;
}
