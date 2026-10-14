// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/ring.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

audio_ring_t g_ring_net_to_usb;
audio_ring_t g_ring_usb_to_net;

#define FRAME_BYTES (UI_CHANNELS * sizeof(int32_t))

static void alloc_ring(audio_ring_t *r)
{
    r->buf = heap_caps_aligned_calloc(64, RING_FRAMES, FRAME_BYTES,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(r->buf ? ESP_OK : ESP_ERR_NO_MEM);
    atomic_store(&r->head, 0);
    atomic_store(&r->underruns, 0);
    atomic_store(&r->late, 0);
}

void ring_init(void)
{
    alloc_ring(&g_ring_net_to_usb);
    alloc_ring(&g_ring_usb_to_net);
}

void IRAM_ATTR ring_read_frames(audio_ring_t *r, uint64_t frame, int32_t *dst, size_t n)
{
    while (n) {
        size_t idx = (size_t)(frame & RING_MASK);
        size_t run = RING_FRAMES - idx;
        if (run > n) run = n;
        memcpy(dst, &r->buf[idx * UI_CHANNELS], run * FRAME_BYTES);
        dst += run * UI_CHANNELS;
        frame += run;
        n -= run;
    }
}

void IRAM_ATTR ring_clear_frames(audio_ring_t *r, uint64_t frame, size_t n)
{
    while (n) {
        size_t idx = (size_t)(frame & RING_MASK);
        size_t run = RING_FRAMES - idx;
        if (run > n) run = n;
        memset(&r->buf[idx * UI_CHANNELS], 0, run * FRAME_BYTES);
        frame += run;
        n -= run;
    }
}

void IRAM_ATTR ring_read_frames_and_clear(audio_ring_t *r, uint64_t frame, int32_t *dst, size_t n)
{
    while (n) {
        size_t idx = (size_t)(frame & RING_MASK);
        size_t run = RING_FRAMES - idx;
        if (run > n) run = n;
        int32_t *src = &r->buf[idx * UI_CHANNELS];
        memcpy(dst, src, run * FRAME_BYTES);
        memset(src, 0, run * FRAME_BYTES);
        dst += run * UI_CHANNELS;
        frame += run;
        n -= run;
    }
}

// The head is the published "frames up to here are written" mark: it must
// only ever advance. A re-anchor or out-of-order write below the current
// head (engine overrun re-anchor after a long host absence) must not regress
// it — ring_valid() would then declare previously valid ranges invalid, and
// slots being written concurrently are inside the valid window anyway
// (docs/13 §6: was stored unconditionally).
static void IRAM_ATTR ring_publish_head(audio_ring_t *r, uint64_t end)
{
    uint64_t old = atomic_load_explicit(&r->head, memory_order_relaxed);
    if (end > old) {
        atomic_store_explicit(&r->head, end, memory_order_release);
    }
}

void IRAM_ATTR ring_write_frames(audio_ring_t *r, uint64_t frame, const int32_t *src, size_t n)
{
    uint64_t end = frame + n;
    while (n) {
        size_t idx = (size_t)(frame & RING_MASK);
        size_t run = RING_FRAMES - idx;
        if (run > n) run = n;
        memcpy(&r->buf[idx * UI_CHANNELS], src, run * FRAME_BYTES);
        src += run * UI_CHANNELS;
        frame += run;
        n -= run;
    }
    ring_publish_head(r, end);
}

void IRAM_ATTR ring_write_silence(audio_ring_t *r, uint64_t frame, size_t n)
{
    uint64_t end = frame + n;
    while (n) {
        size_t idx = (size_t)(frame & RING_MASK);
        size_t run = RING_FRAMES - idx;
        if (run > n) run = n;
        memset(&r->buf[idx * UI_CHANNELS], 0, run * FRAME_BYTES);
        frame += run;
        n -= run;
    }
    ring_publish_head(r, end);
}
