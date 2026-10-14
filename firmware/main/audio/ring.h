// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio rings indexed by the 64-bit media frame index.
//
// A frame index is the network media clock in samples:
//     frame = PTP_time_ns * UI_SAMPLE_RATE / 1e9      (never wraps in practice)
// Network packets carry exactly this number, so a packet's samples are written
// to (or read from) the slot its timestamp names. The USB side reads and writes
// at positions derived from the same clock. Nothing copies between the two
// sides: each ring has one producer side and one consumer side.
//
// Layout: interleaved frames of UI_CHANNELS int32 samples. Samples are
// left-justified (24-bit audio in bits 31..8, low byte 0), i.e. the USB
// 4-byte subslot format, so USB transfers are plain memcpy.
//
//   g_ring_net_to_usb : producers = AoIP RX flows (any channel subset, any
//                       order); consumer = USB IN. The consumer zeroes what
//                       it read, so a stopped flow gives silence, not a loop.
//   g_ring_usb_to_net : producer = USB OUT (whole frames, in order);
//                       consumers = AoIP TX flows (any number, any subset).
//                       Validity is tracked by the producer's head index.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

// 512 frames = 10.7 ms; power of 2. Sized by the internal-RAM budget
// (docs/01-architecture.md §8: 2 rings x 64 KB at 32 ch next to 150 KB of
// EMAC DMA in ~390 KB of heap). Bounds that follow from it: RX transmitter
// latency up to our latency + RING/2 (5.3 ms more, flows_rx.c RX_MAX_SHIFT),
// USB OUT lead up to RING/2 (ENG_OUT_LEAD_MAX), loopback delay 192.
#define RING_FRAMES     512u
#define RING_MASK       (RING_FRAMES - 1u)

typedef struct {
    int32_t *buf;                               // RING_FRAMES * UI_CHANNELS
    // Producer head: frames [head - RING_FRAMES + guard, head) are valid
    // (usb_to_net only). UINT64_MAX-style "never written" is 0.
    _Atomic uint64_t head;
    // Counters (telemetry ring_*; relaxed 32-bit atomics, ISR-safe)
    //   usb_to_net.underruns: AoIP TX packets built from frames USB OUT had
    //                         not written (sent as silence)
    //   net_to_usb.underruns: USB IN read position re-anchored (frames lost)
    //   net_to_usb.late:      AoIP RX packets that arrived after their
    //                         playout deadline (dropped)
    _Atomic uint32_t underruns;
    _Atomic uint32_t late;
} audio_ring_t;

extern audio_ring_t g_ring_net_to_usb;
extern audio_ring_t g_ring_usb_to_net;

// Allocate both rings in internal, DMA-capable RAM. Call once at boot.
void ring_init(void);

static inline int32_t *ring_frame(audio_ring_t *r, uint64_t frame)
{
    return &r->buf[(size_t)(frame & RING_MASK) * UI_CHANNELS];
}

// ---- whole-frame access (USB side) ----------------------------------------

// Copy n frames starting at `frame` without consuming (stage before arm).
void ring_read_frames(audio_ring_t *r, uint64_t frame, int32_t *dst, size_t n);

// Zero n frames without publishing anything (commit of ring_read_frames).
void ring_clear_frames(audio_ring_t *r, uint64_t frame, size_t n);

// Copy n frames starting at `frame` out of the ring (wrap handled), then
// zero the source (net_to_usb semantics).
void ring_read_frames_and_clear(audio_ring_t *r, uint64_t frame, int32_t *dst, size_t n);

// Copy n whole frames into the ring at `frame` and advance head to frame+n.
void ring_write_frames(audio_ring_t *r, uint64_t frame, const int32_t *src, size_t n);

// Fill n frames at `frame` with zeros and advance head (USB OUT not streaming).
void ring_write_silence(audio_ring_t *r, uint64_t frame, size_t n);

// ---- per-channel access (network side) ------------------------------------

// True if frames [frame, frame+n) were written by the producer and have not
// yet been overwritten (usb_to_net). Readers emit silence otherwise.
static inline bool ring_valid(audio_ring_t *r, uint64_t frame, size_t n)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    return frame + n <= head && head - frame <= RING_FRAMES - 64u;
}
