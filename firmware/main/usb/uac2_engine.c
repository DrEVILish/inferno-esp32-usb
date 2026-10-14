// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB audio clock/position engine (see uac2_engine.h and docs/10-usb.md).
#include "usb/uac2_engine.h"

#include <string.h>

#include "audio/ring.h"
#include "clock/mediaclock.h"

#ifdef UAC2_HOST_SIM
#define ENG_IRAM
#else
#include "esp_attr.h"
#define ENG_IRAM IRAM_ATTR
#endif

#define FRAME_BYTES UAC2_FRAME_BYTES

// Which ring USB IN reads, and how:
//   production           g_ring_net_to_usb, read-and-clear
//   USB loopback test    g_ring_usb_to_net, non-clearing (network TX reads it too)
//   network loopback     neither: AoIP TX reads g_ring_net_to_usb itself
//                        (non-clearing), so USB IN sends silence and never
//                        clears that ring.
#if defined(CONFIG_USBINF_TEST_USB_LOOPBACK) || defined(CONFIG_USBINF_TEST_NET_LOOPBACK)
#define ENG_IN_CLEARS_NET_RING 0
#else
#define ENG_IN_CLEARS_NET_RING 1
#endif

// ---------------------------------------------------------------------------
// Ring helpers that ring.h does not provide (kept local to usb/)
// ---------------------------------------------------------------------------

#if ENG_IN_CLEARS_NET_RING
// Zero n frames at `frame` without touching head (net_to_usb discard).
static ENG_IRAM void ring_clear(audio_ring_t *rg, uint64_t frame, size_t n)
{
    while (n) {
        size_t idx = (size_t)(frame & RING_MASK);
        size_t run = RING_FRAMES - idx;
        if (run > n) run = n;
        memset(&rg->buf[idx * UI_CHANNELS], 0, run * FRAME_BYTES);
        frame += run;
        n -= run;
    }
}
#endif

#ifdef CONFIG_USBINF_TEST_USB_LOOPBACK
// Copy n frames out of the ring without clearing (usb_to_net is read by the
// network TX side too). Frames the producer has not written read as zero.
static ENG_IRAM bool ring_read_keep(audio_ring_t *rg, uint64_t frame, int32_t *dst, size_t n)
{
    if (!ring_valid(rg, frame, n)) {
        memset(dst, 0, n * FRAME_BYTES);
        return false;
    }
    while (n) {
        size_t idx = (size_t)(frame & RING_MASK);
        size_t run = RING_FRAMES - idx;
        if (run > n) run = n;
        memcpy(dst, &rg->buf[idx * UI_CHANNELS], run * FRAME_BYTES);
        dst += run * UI_CHANNELS;
        frame += run;
        n -= run;
    }
    return true;
}
#endif

// ---------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------

static inline uint64_t in_target(const uac2_engine_t *e)
{
    return e->sof_f > (uint64_t)ENG_IN_DELAY ? e->sof_f - (uint64_t)ENG_IN_DELAY : 0;
}

// Restart OUT at now + lead. Silence is written from a little behind 'now'
// up to the new w so that no stale frame from an earlier lap of the ring is
// left looking valid below head.
static ENG_IRAM void reanchor_out(uac2_engine_t *e, uint64_t now)
{
    uint64_t start = now > ENG_OUT_BACKFILL ? now - ENG_OUT_BACKFILL : 0;
    uint64_t end = now + (uint64_t)ENG_OUT_LEAD;
    ring_write_silence(&g_ring_usb_to_net, start, (size_t)(end - start));
    e->w = end;
    e->lead_q8 = ENG_OUT_LEAD << 8;
}

// Restart IN reading at `pos`. Everything in the ring that lies behind pos
// (all of it except the newest ~128 frames that network RX may already have
// filled ahead of us) is cleared so that no stale lap is ever played.
static ENG_IRAM void reanchor_in(uac2_engine_t *e, uint64_t pos)
{
#if ENG_IN_CLEARS_NET_RING
    const size_t n = RING_FRAMES - 128u;
    uint64_t start = pos > n ? pos - n : 0;
    ring_clear(&g_ring_net_to_usb, start, (size_t)(pos - start));
#endif
    e->r = pos;
    e->st.in_resyncs++;
}

bool ENG_IRAM eng_out_active(const uac2_engine_t *e)
{
    return e->out_alt && e->out_seen && (e->uf - e->last_out_uf) <= ENG_OUT_IDLE_UF;
}

int32_t eng_out_lead(const uac2_engine_t *e)
{
    if (eng_out_active(e)) {
        return e->lead_q8 / 256;
    }
    return (int32_t)(int64_t)(e->w - e->sof_f);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void eng_init(uac2_engine_t *e)
{
    memset(e, 0, sizeof *e);
    e->fb_raw_q24 = ENG_FB_NOMINAL_Q24;
    e->fb_q24 = ENG_FB_NOMINAL_Q24;
    e->lead_q8 = ENG_OUT_LEAD << 8;
    eng_latency_window_reset(e);
}

void eng_latency_window_reset(uac2_engine_t *e)
{
    e->st.out_hr_min = INT32_MAX;
    e->st.out_hr_max = INT32_MIN;
}

void eng_bus_reset(uac2_engine_t *e)
{
    e->have_sof = false;
    e->hist_n = 0;
    e->out_alt = false;
    e->out_seen = false;
    e->in_streaming = false;
}

void eng_out_alt(uac2_engine_t *e, bool on)
{
    e->out_alt = on;
    e->out_seen = false;
}

void eng_in_alt(uac2_engine_t *e, bool on)
{
    (void)on;
    // IN streaming starts with the first packet prepared; stopping hands the
    // ring back to the idle discard in eng_sof() with r continuous.
    e->in_streaming = false;
}

// ---------------------------------------------------------------------------
// SOF: timestamp history -> feedback, idle OUT silence, idle IN discard
// ---------------------------------------------------------------------------

static ENG_IRAM void hist_restart(uac2_engine_t *e)
{
    e->hist_n = 0;
    e->blk_valid = false;
    e->blk_start_uf = e->uf;
    e->pred_outliers = 0;
}

static ENG_IRAM void fb_update(uac2_engine_t *e)
{
    if (e->hist_n >= 2) {
        uint32_t newest = (e->hist_head + ENG_FB_HIST - 1u) % ENG_FB_HIST;
        uint32_t oldest = (e->hist_head + ENG_FB_HIST - e->hist_n) % ENG_FB_HIST;
        uint64_t duf = e->hist[newest].uf - e->hist[oldest].uf;
        uint64_t dns = e->hist[newest].ns - e->hist[oldest].ns;
        if (duf) {
            e->ns_per_uf_q16 = (dns << 16) / duf;
            if (e->hist_n >= ENG_FB_MIN_ENTRIES) {
                e->fb_raw_q24 = eng_fb_q24_from_span(dns, duf);
            }
        }
    }

    int64_t ppb = 0;
    if (eng_out_active(e)) {
        int64_t err_q8 = (int64_t)e->lead_q8 - ((int64_t)ENG_OUT_LEAD << 8);
        // lead too large -> host is ahead -> ask for fewer frames
        ppb = -(err_q8 * ENG_PHASE_GAIN_PPB) / 256;
        if (ppb > ENG_PHASE_MAX_PPB) ppb = ENG_PHASE_MAX_PPB;
        if (ppb < -ENG_PHASE_MAX_PPB) ppb = -ENG_PHASE_MAX_PPB;
    }
    e->phase_ppb = (int32_t)ppb;
    e->fb_q24 = (uint32_t)((int64_t)e->fb_raw_q24 + ((int64_t)e->fb_raw_q24 * ppb) / 1000000000);
}

void ENG_IRAM eng_sof(uac2_engine_t *e, uint64_t t_ns, uint32_t epoch, uint32_t fn)
{
    fn &= 0x3FFFu;
    e->st.sof_count++;

    uint32_t duf = 1;
    bool restart = false, stepped = false;
    if (!e->have_sof) {
        restart = true;
        stepped = (epoch != e->epoch);
    } else if (epoch != e->epoch) {
        restart = stepped = true;
    } else {
        if (t_ns < e->sof_ns || t_ns - e->sof_ns > ENG_SOF_GAP_NS) {
            restart = true;
            e->st.sof_gaps++;
        } else {
            // Microframes since the last SOF interrupt: from the DWC2 frame
            // number (HS: microframe number, 14 bits), cross-checked against
            // the timestamps in case the core reports something else.
            uint32_t duf_fn = (fn - e->last_fn) & 0x3FFFu;
            uint32_t duf_t;
            if (e->hist_n >= ENG_PRED_MIN_ENTRIES && t_ns > e->sof_est_ns) {
                // From the fitted (latency-free) previous SOF: latency only
                // adds, so round with a one-sided margin.
                uint64_t per = e->ns_per_uf_q16 >> 16;
                duf_t = (uint32_t)((t_ns - e->sof_est_ns + per / 4u) / per);
            } else {
                duf_t = (uint32_t)((t_ns - e->sof_ns + 62500u) / 125000u);
            }
            if (duf_fn == 0 && duf_t == 0) {
                return;                 // second interrupt for the same SOF
            }
            if (duf_fn == 0 || duf_fn > duf_t + 1u || duf_fn + 1u < duf_t) {
                e->st.sof_fn_bad++;
                duf = duf_t ? duf_t : 1u;
            } else {
                duf = duf_fn;
            }
            if (duf > 1) {
                e->st.sof_missed += duf - 1u;
            }
        }
    }
    if (restart) {
        // Unknown time elapsed / clock stepped: OUT restarts from scratch on
        // its next packet (the idle silence writer takes over meanwhile).
        duf = 1;
        e->out_seen = false;
    }
    e->uf += duf;
    e->last_fn = fn;
    e->sof_ns = t_ns;
    e->have_sof = true;
    if (restart) {
        hist_restart(e);
    }

    // SOF time on the media clock. The raw ISR timestamp carries interrupt
    // latency jitter; once a few blocks exist, use the time predicted by the
    // fitted SOF line (last envelope point + fitted period), so that the
    // per-microframe IN frame count follows a clean line (5/6/7 only).
    uint64_t sof_ns_est = t_ns;
    e->sof_timely = (duf == 1);
    if (!restart && e->hist_n >= ENG_PRED_MIN_ENTRIES) {
        uint32_t newest = (e->hist_head + ENG_FB_HIST - 1u) % ENG_FB_HIST;
        uint64_t k = e->uf - e->hist[newest].uf;
        uint64_t pred = e->hist[newest].ns + ((k * e->ns_per_uf_q16 + 0x8000u) >> 16);
        int64_t dev = (int64_t)(t_ns - pred);
        if (dev > ENG_PRED_EARLY_NS) {
            e->st.sof_late++;
            e->sof_timely = false;
        }
        if (dev < -(int64_t)ENG_PRED_EARLY_NS || dev > (int64_t)ENG_PRED_LATE_NS) {
            if (++e->pred_outliers >= ENG_PRED_OUTLIERS) {
                // The clock moved under us without an epoch change: refit.
                e->st.sof_refits++;
                hist_restart(e);
            } else {
                sof_ns_est = pred;
            }
        } else {
            e->pred_outliers = 0;
            sof_ns_est = pred;
        }
    }
    e->sof_est_ns = sof_ns_est;
    e->sof_f = mc_ns_to_frames(sof_ns_est);

    if (stepped) {
        e->epoch = epoch;
        e->st.epoch_changes++;
        reanchor_out(e, e->sof_f);
        reanchor_in(e, in_target(e));
    }

    // Feedback measurement: per block of ENG_FB_HIST_UF microframes keep the
    // SOF with the smallest (timestamp - nominal time), i.e. the least ISR
    // latency (the lower envelope); push it into the history at block end.
    int64_t key = (int64_t)(t_ns - e->uf * 125000ull);
    if (!e->blk_valid || key < e->blk_key) {
        e->blk_valid = true;
        e->blk_key = key;
        e->blk_uf = e->uf;
        e->blk_ns = t_ns;
    }
    if (e->uf - e->blk_start_uf >= ENG_FB_HIST_UF) {
        e->hist[e->hist_head].uf = e->blk_uf;
        e->hist[e->hist_head].ns = e->blk_ns;
        e->hist_head = (e->hist_head + 1u) % ENG_FB_HIST;
        if (e->hist_n < ENG_FB_HIST) e->hist_n++;
        e->blk_valid = false;
        e->blk_start_uf = e->uf;
        fb_update(e);
    }

    // OUT idle: keep the TX ring filled with silence up to now + lead.
    if (!eng_out_active(e)) {
        uint64_t target = e->sof_f + (uint64_t)ENG_OUT_LEAD;
        int64_t d = (int64_t)(target - e->w);
        if (d > 0 && d <= ENG_OUT_SILENCE_MAX) {
            ring_write_silence(&g_ring_usb_to_net, e->w, (size_t)d);
            e->w = target;
        } else if (d > ENG_OUT_SILENCE_MAX || d < -(int64_t)(ENG_OUT_LEAD_MAX - ENG_OUT_LEAD)) {
            reanchor_out(e, e->sof_f);
        }
        // else: w a little ahead of target (host just stopped) - let 'now' catch up
    }

    // IN idle: discard network RX that nobody plays (and keep r continuous).
    if (!e->in_streaming) {
        uint64_t target = in_target(e);
        int64_t d = (int64_t)(target - e->r);
        if (d >= 0 && d <= ENG_IN_RESYNC) {
#if ENG_IN_CLEARS_NET_RING
            ring_clear(&g_ring_net_to_usb, e->r, (size_t)d);
#endif
            e->r = target;
        } else {
            reanchor_in(e, target);
        }
    }
}

// ---------------------------------------------------------------------------
// OUT (host playback) -> g_ring_usb_to_net
// ---------------------------------------------------------------------------

void ENG_IRAM eng_out_packet(uac2_engine_t *e, uint64_t now, uint64_t arm_uf,
                             const void *data, uint32_t nbytes)
{
    // Packet slots are counted by arrival microframe; lost packets = gap
    // beyond 1. The host sends every microframe, so a transfer armed in
    // arm_uf receives the packet of microframe arm_uf + 1 however late this
    // interrupt runs. Exception: the ISR is demonstrably on time (this microframe's SOF
    // was handled promptly) yet two or more microframes have passed since
    // arming - then earlier packets were lost on the bus/by parity and this
    // one arrived in the current microframe.
    uint64_t a_uf = arm_uf + 1u;
    if (e->sof_timely && e->uf >= a_uf + 2u) {
        a_uf = e->uf;
    }
    const bool have_prev = e->out_seen;
    const uint64_t gap = a_uf - e->last_arr_uf;
    e->last_arr_uf = a_uf;

    if (nbytes == 0) {
        e->st.out_zlp++;
        return;
    }
    uint32_t n = nbytes / FRAME_BYTES;
    if (nbytes % FRAME_BYTES) {
        e->st.out_bad_len++;
    }
    if (n == 0) {
        return;
    }

    // Media frame at the SOF of the arrival microframe (latency-free), so the
    // lead does not depend on when this interrupt got to run.
    uint64_t f_arr = now;
    int64_t late_uf = (int64_t)(e->uf - a_uf);
    if (late_uf >= 0 && late_uf <= 64) {
        f_arr = e->sof_f - (((uint64_t)late_uf * e->fb_q24 + (1u << 23)) >> 24);
    }
    int64_t lead = (int64_t)(e->w - f_arr);

    if (!eng_out_active(e) || !have_prev) {
        // (Re)start. The idle silence writer has kept w at ~now + lead, so the
        // stream normally continues seamlessly from there.
        if (lead < ENG_OUT_LEAD_MIN || lead > ENG_OUT_LEAD_MAX) {
            reanchor_out(e, f_arr);
            lead = ENG_OUT_LEAD;
        }
        e->lead_q8 = (int32_t)(lead * 256);
    } else {
        // Lost packets: fill with silence so that the host's sample stream
        // stays aligned to the media clock. The host sized the lost packets
        // from our feedback, so (gap - 1) * feedback is the estimate; the
        // drop of the lead below its average cross-checks it (and catches a
        // loss the slot count cannot see).
        int64_t measured = (e->lead_q8 + 128) / 256 - lead;
        int64_t fill = 0;
        if (gap >= 2) {
            int64_t model = (int64_t)(((gap - 1u) * (uint64_t)e->fb_q24 + (1u << 23)) >> 24);
            if (measured - model <= ENG_OUT_GAP_DETECT && model - measured <= ENG_OUT_GAP_DETECT) {
                fill = model;
            } else if (measured >= ENG_OUT_GAP_DETECT) {
                fill = measured;
            }
            // A loss estimate beyond the lead window is a discontinuity, not
            // a gap (host suspend/resume, several lost completions): skip the
            // fill and let the re-anchor below reset positions. Unbounded
            // gap*fb fills would jump the ring head far past now and throw
            // every TX consumer out of ring_valid (docs/13 §6).
            if (fill > (int64_t)ENG_OUT_LEAD_MAX) fill = 0;
        } else if (measured > (int64_t)UAC2_NOMINAL_FPU - ENG_OUT_GAP_DETECT + 1 &&
                   measured <= 2 * (int64_t)UAC2_MAX_FPU) {
            fill = measured;
        }
        if (fill > 0) {
            ring_write_silence(&g_ring_usb_to_net, e->w, (size_t)fill);
            e->w += (uint64_t)fill;
            lead += fill;
            e->st.out_gap_fills++;
            e->st.out_gap_frames += (uint32_t)fill;
        }
        if (lead < ENG_OUT_LEAD_MIN || (int64_t)(e->w - now) < ENG_OUT_REAL_MIN) {
            e->st.out_underruns++;
            reanchor_out(e, f_arr);
            lead = ENG_OUT_LEAD;
            e->lead_q8 = (int32_t)(lead * 256);
        } else if (lead > ENG_OUT_LEAD_MAX) {
            e->st.out_overruns++;
            reanchor_out(e, f_arr);
            lead = ENG_OUT_LEAD;
            e->lead_q8 = (int32_t)(lead * 256);
        } else {
            // IIR, 128 packets (16 ms) time constant
            e->lead_q8 += (int32_t)((lead * 256 - e->lead_q8) / 128);
        }
    }

    const int64_t hr = (int64_t)(e->w - now);
    if (hr < e->st.out_hr_min) e->st.out_hr_min = (int32_t)hr;
    if (hr > e->st.out_hr_max) e->st.out_hr_max = (int32_t)hr;
    ring_write_frames(&g_ring_usb_to_net, e->w, (const int32_t *)data, n);
    e->w += n;
    e->last_out_uf = e->uf;
    e->out_seen = true;
    e->st.out_packets++;
    e->st.out_frames += n;
}

// ---------------------------------------------------------------------------
// IN (host capture) <- g_ring_net_to_usb
// ---------------------------------------------------------------------------

// Split of eng_in_packet: stage renders [r, r+n) without consuming, so the
// caller can arm the endpoint first and COMMIT (advance + clear + counters)
// only when the transfer was actually accepted — a failed usbd_edpt_xfer
// must not silently drop the frames; the next call re-renders the same
// range (docs/13 §6: was consume-then-maybe-fail).
uint32_t ENG_IRAM eng_stage_in_packet(uac2_engine_t *e, void *dst)
{
    e->in_streaming = true;

    // Frames whose playout time has passed at the last SOF. Consecutive
    // packets therefore carry exactly the media-clock advance between SOFs.
    uint64_t target = in_target(e);
    int64_t d = (int64_t)(target - e->r);
    if (d < 0 || d > ENG_IN_RESYNC) {
        // before the first frames exist the target can be < the nominal
        // packet: clamp so the re-anchor start does not underflow
        reanchor_in(e, target > UAC2_NOMINAL_FPU ? target - UAC2_NOMINAL_FPU : 0);
        e->st.in_underruns++;
        atomic_fetch_add_explicit(&g_ring_net_to_usb.underruns, 1u, memory_order_relaxed);
        d = UAC2_NOMINAL_FPU;
    }
    uint32_t n = (uint32_t)d;
    if (n > UAC2_MAX_FPU) {
        n = UAC2_MAX_FPU;
        e->st.in_catchup++;
    }
    if (n) {
#ifdef CONFIG_USBINF_TEST_USB_LOOPBACK
        if (!ring_read_keep(&g_ring_usb_to_net, e->r, (int32_t *)dst, n)) {
            e->st.in_underruns++;
        }
#elif defined(CONFIG_USBINF_TEST_NET_LOOPBACK)
        memset(dst, 0, n * FRAME_BYTES);
#else
        ring_read_frames(&g_ring_net_to_usb, e->r, (int32_t *)dst, n);
#endif
    }
    return n * FRAME_BYTES;
}

void ENG_IRAM eng_commit_in_packet(uac2_engine_t *e, uint32_t bytes)
{
    const uint32_t n = bytes / FRAME_BYTES;
    if (!n) return;
#if !defined(CONFIG_USBINF_TEST_USB_LOOPBACK) && !defined(CONFIG_USBINF_TEST_NET_LOOPBACK)
    ring_clear_frames(&g_ring_net_to_usb, e->r, n);
#endif
    e->r += n;
    e->st.in_packets++;
    e->st.in_frames += n;
}

// Stage + commit in one go (host simulation, tools/usb_engine_sim.c).
uint32_t ENG_IRAM eng_in_packet(uac2_engine_t *e, void *dst)
{
    const uint32_t bytes = eng_stage_in_packet(e, dst);
    eng_commit_in_packet(e, bytes);
    return bytes;
}

// ---------------------------------------------------------------------------
// Feedback
// ---------------------------------------------------------------------------

uint32_t ENG_IRAM eng_fb_q16(uac2_engine_t *e)
{
    uint32_t q24 = e->fb_q24;
    uint32_t s = e->fb_dither + (q24 & 0xFFu);
    e->fb_dither = s & 0xFFu;
    return (q24 >> 8) + (s >> 8);
}
