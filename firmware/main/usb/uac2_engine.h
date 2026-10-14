// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB audio clock/position engine: pure C, no TinyUSB, no ESP-IDF calls, so it
// can be compiled and exercised on a PC (tools/usb_engine_sim.c).
//
// It owns
//   - the feedback value: media frames per USB microframe, measured from the
//     media-clock timestamps of SOFs over a sliding ~1 s window, plus a
//     bounded (+-10 ppm) phase term that holds the OUT write lead;
//   - the OUT write position w into g_ring_usb_to_net (silence while the host
//     is not streaming, host frames while it is);
//   - the IN read position r out of g_ring_net_to_usb (or g_ring_usb_to_net
//     in the USB loopback test build) and the per-microframe IN frame count.
//
// All entry points must be called from one context at a time (uac2.c calls
// them from the USB ISR or under its spinlock).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef UAC2_HOST_SIM
#include "sdkconfig.h"          // CONFIG_USBINF_TEST_USB_LOOPBACK
#endif
#include "app_config.h"
#include "usb/uac2_defs.h"

// ---- tuning ---------------------------------------------------------------
#define ENG_FB_HIST             128u    // history entries (ring)
#define ENG_FB_HIST_UF          64u     // microframes between entries (8 ms) -> window 127*8 ms ~ 1.02 s
#define ENG_FB_MIN_ENTRIES      17u     // trust the measurement after 16 intervals (128 ms)
#define ENG_FB_NOMINAL_Q24      ((uint32_t)UAC2_NOMINAL_FPU << 24)
#define ENG_PHASE_GAIN_PPB      1000    // phase term: 1 ppm per frame of lead error ...
#define ENG_PHASE_MAX_PPB       10000   // ... clamped to +-10 ppm
#define ENG_OUT_LEAD            ((int32_t)UI_USB_OUT_LEAD_FRAMES)
// lead below this -> host late: re-anchor (underrun). Half the target lead,
// at least 4 frames (the 12-frame profiles: 6).
#define ENG_OUT_LEAD_MIN        (ENG_OUT_LEAD / 2 > 4 ? ENG_OUT_LEAD / 2 : 4)
// ... or the frames about to be written are this close to (or behind) the
// real media clock at the write: the packetizer may already have sent them.
// Lead 48: 9 frames; lead 12: 0 (arrival takes one microframe, 6 frames).
#define ENG_OUT_REAL_MIN        ((ENG_OUT_LEAD_MIN - 6) / 2)
// lead above this -> host early: re-anchor (overrun). Must stay below the
// TX reader's validity window (RING_FRAMES - 64 - fpp).
#define ENG_OUT_LEAD_MAX        ((int32_t)RING_FRAMES / 2)
#define ENG_OUT_IDLE_UF         32u     // no OUT packet for 4 ms -> not streaming, write silence
#define ENG_OUT_BACKFILL        32u     // silence written behind 'now' on a re-anchor
#define ENG_OUT_GAP_DETECT      3       // lead drop (frames) that confirms lost OUT packets
#define ENG_OUT_SILENCE_MAX     64      // max silence frames per SOF before re-anchoring
#define ENG_IN_MARGIN           ((int32_t)UI_USB_IN_MARGIN_FRAMES)
#define ENG_IN_RESYNC           64      // IN backlog beyond this -> re-anchor
#define ENG_SOF_GAP_NS          100000000ull    // 100 ms without SOF -> restart measurement
#define ENG_PRED_MIN_ENTRIES    3u      // fitted SOF times used once 2 intervals (16 ms) exist
#define ENG_PRED_EARLY_NS       50000   // timestamp earlier than the fit by this: suspicious
#define ENG_PRED_LATE_NS        2000000 // ... or later than this (ISR latency cannot explain it)
#define ENG_PRED_OUTLIERS       8u      // that many in a row -> refit

// IN reads frames < (fitted SOF frame) - ENG_IN_DELAY. The fitted SOF time
// is the lower envelope of the ISR timestamps, i.e. never later than the
// actual read, so a frame f is always read at media time > f + UI_NET_LATENCY
// + margin: the bound AoIP RX late-detection relies on.
#ifdef CONFIG_USBINF_TEST_USB_LOOPBACK
#define ENG_IN_DELAY            ((int64_t)UI_LOOPBACK_DELAY_FRAMES)
#else
#define ENG_IN_DELAY            ((int64_t)UI_NET_LATENCY_FRAMES + ENG_IN_MARGIN)
#endif

typedef struct {
    uint32_t sof_count;         // SOF interrupts handled
    uint32_t sof_missed;        // microframes whose SOF interrupt we never saw
    uint32_t sof_gaps;          // SOF stream restarts (suspend, unplug, >100 ms gap)
    uint32_t epoch_changes;     // media clock steps seen
    uint32_t sof_late;          // SOF timestamps > 50 us after the fitted SOF time
    uint32_t sof_fn_bad;        // DSTS frame number disagreed with the timestamps
    uint32_t sof_refits;        // fitted SOF line abandoned (clock moved without an epoch change)
    uint32_t out_packets, out_frames;
    uint32_t out_overruns;      // re-anchors: host ahead (lead > max)
    uint32_t out_underruns;     // re-anchors: host behind (lead < min)
    uint32_t out_gap_fills, out_gap_frames;     // lost OUT packets replaced by silence
    uint32_t out_bad_len;       // OUT packets not a whole number of frames
    uint32_t out_zlp;           // zero-length OUT packets
    uint32_t in_packets, in_frames;
    uint32_t in_underruns;      // IN re-anchors + loopback reads of unwritten frames
    uint32_t in_catchup;        // IN packets capped at 7 frames while behind
    uint32_t in_resyncs;        // all IN re-anchors (incl. idle/boot/epoch)
    // latency window (reset by the reader, uac2_get_stats): headroom of the
    // host's OUT data at the moment it is written, first frame - real now,
    // in frames. This is the device's USB->network residence ahead of the
    // packet timestamp (the OUT lead as actually achieved); < 0 = too late.
    int32_t  out_hr_min, out_hr_max;
} uac2_eng_stats_t;

typedef struct {
    // clock / SOF
    bool     have_sof;
    uint32_t last_fn;           // DSTS frame number (14 bits, HS microframes)
    uint64_t uf;                // extended microframe counter
    uint64_t sof_ns;            // media time of the last SOF (raw ISR timestamp)
    uint64_t sof_est_ns;        // ... on the fitted SOF line (= sof_ns until fitted)
    uint64_t sof_f;             // media frame of the last SOF
    uint32_t epoch;
    bool     sof_timely;        // last SOF handled promptly (no missed SOF, small latency)

    // feedback: lower envelope of SOF timestamps per block -> history
    bool     blk_valid;
    int64_t  blk_key;           // min over the block of (ns - uf * 125000)
    uint64_t blk_uf, blk_ns, blk_start_uf;
    struct { uint64_t uf, ns; } hist[ENG_FB_HIST];
    uint32_t hist_head, hist_n;
    uint64_t ns_per_uf_q16;     // fitted SOF period in media ns, 48.16
    uint32_t pred_outliers;     // consecutive SOFs far off the fitted line
    uint32_t fb_raw_q24;        // measured frames/microframe, 8.24
    uint32_t fb_q24;            // incl. phase term
    uint32_t fb_dither;         // sub-LSB error carried between feedback packets
    int32_t  phase_ppb;

    // OUT
    bool     out_alt, out_seen;
    uint64_t w;                 // next frame index to write into g_ring_usb_to_net
    uint64_t last_out_uf;       // microframe the last OUT packet was processed in
    uint64_t last_arr_uf;       // estimated arrival microframe of the last OUT packet
    int32_t  lead_q8;           // smoothed (w - now) before each write, 24.8 frames

    // IN
    bool     in_streaming;
    uint64_t r;                 // next frame index to read

    uac2_eng_stats_t st;
} uac2_engine_t;

void eng_init(uac2_engine_t *e);
void eng_latency_window_reset(uac2_engine_t *e);    // out_hr_min/max := empty

// USB bus reset / deconfigure: streams off, SOF tracking restarts. The rate
// measurement and the ring positions carry on.
void eng_bus_reset(uac2_engine_t *e);

// One SOF interrupt. t_ns = media time latched in the ISR, fn = DSTS SOFFN.
void eng_sof(uac2_engine_t *e, uint64_t t_ns, uint32_t epoch, uint32_t fn);

void eng_out_alt(uac2_engine_t *e, bool on);
void eng_in_alt(uac2_engine_t *e, bool on);

// One received OUT packet (nbytes of interleaved 32 x 4-byte frames).
// arm_uf = e->uf at the time its transfer was armed (eng_uf()).
void eng_out_packet(uac2_engine_t *e, uint64_t now_frames, uint64_t arm_uf,
                    const void *data, uint32_t nbytes);

static inline uint64_t eng_uf(const uac2_engine_t *e) { return e->uf; }

// Fill the next IN packet; returns its length in bytes (frames * 128).
uint32_t eng_in_packet(uac2_engine_t *e, void *dst);

// Next feedback value, 16.16 frames per microframe (dithered to carry the
// sub-LSB part of the 8.24 estimate).
uint32_t eng_fb_q16(uac2_engine_t *e);

// Current OUT lead (w - now) in frames, for telemetry.
int32_t eng_out_lead(const uac2_engine_t *e);
bool    eng_out_active(const uac2_engine_t *e);

// Pure maths, exposed for the host test.
// frames per microframe in 8.24 from a media-time span over n microframes.
static inline uint32_t eng_fb_q24_from_span(uint64_t dt_ns, uint64_t n_uf)
{
    // q24 = dt_ns * fs * 2^24 / (1e9 * n_uf); with 1e9 = 8000 * 125000:
    //     = dt_ns * (fs / 8000 * 2^24) / (125000 * n_uf)
    // fs/8000*2^24 = 100663296 for 48 kHz; dt_ns ~1e9 keeps the product < 2^57.
    const uint64_t k_num = (uint64_t)UAC2_NOMINAL_FPU << 24;
    const uint64_t den = 125000ull * n_uf;
    return (uint32_t)((dt_ns * k_num + den / 2u) / den);
}
