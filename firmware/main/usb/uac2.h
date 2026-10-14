// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB Audio Class 2.0 on the ESP32-P4 High-Speed OTG port.
//
// One 32-ch OUT (host playback) and one 32-ch IN (host capture) stream,
// 48 kHz, 24-bit in 4-byte subslots, asynchronous with an explicit feedback
// endpoint. The device is the clock master: the feedback reports the media
// clock (mediaclock.h) in frames per microframe, so the host plays exactly at
// the network's rate. No sample-rate conversion, no volume/mute, bit-exact.
//
//   OUT: host frames -> g_ring_usb_to_net at position w, where w is anchored
//        to mc_now_frames() + UI_USB_OUT_LEAD_FRAMES when streaming starts;
//        a bounded phase term in the feedback keeps (w - now) at the lead.
//        When the host is not streaming, silence is written at now+lead.
//   IN:  each microframe sends the frames the media clock advanced since the
//        previous SOF, read from g_ring_net_to_usb at
//        now - UI_NET_LATENCY_FRAMES - margin (or from the OUT ring in the
//        USB loopback test build).
//
// Owner: usb/ (USB agent).
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t uac2_start(void);

typedef struct {
    uint32_t sof_count;
    uint32_t out_frames, in_frames;
    uint32_t out_overruns, out_underruns, in_underruns;
    uint32_t feedback_q16;          // last feedback value (frames/microframe, 16.16)
    int32_t  out_lead_frames;       // current (w - now)
    uint8_t  out_streaming, in_streaming;
    // ---- additions (usb/; see docs/10-usb.md for definitions) ----
    uint32_t sof_missed;            // microframes whose SOF interrupt was never seen
    uint32_t sof_late;              // SOF timestamps > 50 us behind the fitted SOF line
    uint32_t sof_gaps;              // SOF stream restarts (suspend/unplug/>100 ms)
    uint32_t sof_fn_bad;            // DWC2 frame number disagreed with timestamps
    uint32_t epoch_changes;         // media clock steps handled (positions re-anchored)
    uint32_t out_packets, in_packets;
    uint32_t out_gap_fills;         // lost OUT packets replaced by silence (events)
    uint32_t out_gap_frames;        // ... frames of silence inserted
    uint32_t out_bad_len;           // OUT packets that were not whole frames
    uint32_t in_catchup;            // IN packets capped at 7 frames while catching up
    uint32_t in_resyncs;            // IN read position re-anchors (incl. boot/epoch)
    uint32_t xfer_errors;           // failed iso transfers (OUT + IN + feedback)
    uint32_t arm_failures;          // usbd_edpt_xfer refused (bus reset races)
    int32_t  fb_phase_ppb;          // current phase term in the feedback
    // latency windows since the previous uac2_get_stats() call (frames;
    // INT32_MAX/INT32_MIN = no sample): see docs/14-latency.md
    int32_t  out_hr_min, out_hr_max;    // OUT data first frame - now at write (achieved lead)
    int32_t  in_age_min, in_age_max;    // now - newest frame in an IN packet when built
    uint8_t  configured, high_speed, out_alt, in_alt;
} uac2_stats_t;
void uac2_get_stats(uac2_stats_t *s);
