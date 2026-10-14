// SPDX-License-Identifier: GPL-3.0-or-later
#include "aoip/wire.h"

#include <string.h>
#include "aoip/proto.h"

void bb_init(bb_t *bb, uint8_t *buf, size_t cap)
{
    bb->b = buf;
    bb->cap = (uint16_t)(cap > 0xFFFFu ? 0xFFFFu : cap);
    bb->wpos = 0;
    bb->len = 0;
    bb->ovf = false;
}

void bb_put(bb_t *bb, const void *data, size_t n)
{
    if (bb->ovf || (size_t)bb->wpos + n > bb->cap) {
        bb->ovf = true;
        return;
    }
    if (data) {
        memcpy(bb->b + bb->wpos, data, n);
    } else {
        memset(bb->b + bb->wpos, 0, n);
    }
    bb->wpos = (uint16_t)(bb->wpos + n);
    if (bb->wpos > bb->len) bb->len = bb->wpos;
}

void bb_zeros(bb_t *bb, size_t n) { bb_put(bb, NULL, n); }
void bb_u8(bb_t *bb, uint8_t v) { bb_put(bb, &v, 1); }

void bb_u16(bb_t *bb, uint16_t v)
{
    uint8_t t[2];
    wr16(t, v);
    bb_put(bb, t, 2);
}

void bb_u32(bb_t *bb, uint32_t v)
{
    uint8_t t[4];
    wr32(t, v);
    bb_put(bb, t, 4);
}

void bb_set_wpos(bb_t *bb, uint16_t pos)
{
    if (pos > bb->cap) {
        bb->ovf = true;
        return;
    }
    // ByteBuffer::set_wpos may move past the end; the gap reads as zeros.
    if (pos > bb->len) {
        memset(bb->b + bb->len, 0, pos - bb->len);
        bb->len = pos;
    }
    bb->wpos = pos;
}

uint16_t bb_str0(bb_t *bb, const char *s)
{
    uint16_t off = bb->wpos;
    bb_put(bb, s, strlen(s));
    bb_u8(bb, 0);
    return off;
}

uint16_t bb_str0_or_0(bb_t *bb, const char *s)
{
    return s ? bb_str0(bb, s) : 0;
}

void bb_align(bb_t *bb, uint16_t a)
{
    while (!bb->ovf && (bb->wpos % a) != 0) bb_u8(bb, 0);
}

bool rd_str0(const uint8_t *buf, size_t len, size_t off, char *out, size_t cap)
{
    if (off >= len || cap == 0) return false;
    size_t n = 0;
    while (off + n < len && buf[off + n] != 0) n++;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, buf + off, n);
    out[n] = 0;
    return true;
}

void rr_header(uint8_t *pkt, uint16_t start_code, uint16_t total, uint16_t seq,
               uint16_t op1, uint16_t op2)
{
    wr16(pkt + 0, start_code);
    wr16(pkt + 2, total);
    wr16(pkt + 4, seq);
    wr16(pkt + 6, op1);
    wr16(pkt + 8, op2);
}

void rr_finish(uint8_t *pkt, const uint8_t *req, uint16_t code, uint16_t total)
{
    // respond_with_code: echo start code, seqnum and opcode1 of the request
    rr_header(pkt, rd16(req + 0), total, rd16(req + 4), rd16(req + 6), code);
}

uint16_t arc_paginate(bb_t *bb, const uint8_t *req_content, size_t req_len,
                      uint8_t space_items, size_t item_size, size_t n_source,
                      arc_item_fn item, void *ctx)
{
    // extract_start_index: payload[2..4] is a 1-based start index
    if (req_len < 4 || (req_content[2] | req_content[3]) == 0) {
        return RR_CODE_TODO_ERROR;
    }
    size_t start = (size_t)rd16(req_content + 2) - 1u;

    bb_u8(bb, space_items);
    bb_u8(bb, 0);
    if (space_items == 0) return RR_CODE_OK;
    const uint16_t items_at = bb->wpos;     // == 2 + HEADER_LENGTH
    bb_zeros(bb, (size_t)space_items * item_size);

    uint16_t item_pos = items_at;
    size_t actual = 0;
    bool have_more = false;
    uint8_t tmp[32];
    if (item_size > sizeof tmp) return RR_CODE_TODO_ERROR;

    for (size_t i = start; i < n_source; i++) {
        if (actual >= space_items) {
            have_more = true;
            break;
        }
        memset(tmp, 0, item_size);
        if (!item(bb, i, tmp, ctx)) continue;
        uint16_t prev_pos = bb->wpos;
        if (!bb->ovf && (size_t)item_pos + item_size <= bb->len) {
            memcpy(bb->b + item_pos, tmp, item_size);
        }
        item_pos = (uint16_t)(item_pos + item_size);
        // Inferno checks the limit after writing the item and then stops
        // without counting it; kept as is for byte-exact replies.
        if (prev_pos >= ARC_PACKET_SIZE_SOFT_LIMIT) {
            have_more = true;
            break;
        }
        actual++;
    }
    if (!bb->ovf) bb->b[items_at - 1] = (uint8_t)actual;
    return have_more ? ARC_CODE_HAVE_MORE : RR_CODE_OK;
}
