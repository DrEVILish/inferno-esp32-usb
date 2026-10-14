// SPDX-License-Identifier: GPL-3.0-or-later
//
// Byte-level helpers shared by the control-plane protocols: big-endian
// access, a growable-in-place byte builder with ByteBuffer semantics
// (write position, absolute offsets), the request/response header and the
// ARC paginated-list serializer (proto_arc.rs serialize_items).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// Byte builder over a caller-owned buffer. Like bytebuffer::ByteBuffer: data
// length is the high-water mark, writes happen at wpos, wpos can be moved.
// On overflow the builder stops writing and sets `ovf`.
typedef struct {
    uint8_t *b;
    uint16_t cap, wpos, len;
    bool ovf;
} bb_t;

void bb_init(bb_t *bb, uint8_t *buf, size_t cap);
void bb_put(bb_t *bb, const void *data, size_t n);
void bb_zeros(bb_t *bb, size_t n);
void bb_u8(bb_t *bb, uint8_t v);
void bb_u16(bb_t *bb, uint16_t v);
void bb_u32(bb_t *bb, uint32_t v);
void bb_set_wpos(bb_t *bb, uint16_t pos);
// Write s + NUL, return the offset it was written at (write_0term_str_to_bytebuffer).
uint16_t bb_str0(bb_t *bb, const char *s);
// As above, but NULL -> 0 and nothing written (write_0term_str_or_0_to_bytebuffer).
uint16_t bb_str0_or_0(bb_t *bb, const char *s);
// Pad with zeros until wpos % a == 0 (align_wpos).
void bb_align(bb_t *bb, uint16_t a);

// read_0term_str_from_buffer: copy the string at `off` (NUL or end of buffer
// terminates it) into out (truncated to cap-1). False if off >= len.
bool rd_str0(const uint8_t *buf, size_t len, size_t off, char *out, size_t cap);

// Request/response header (req_resp.rs). `pkt` holds the full packet,
// header included. rr_finish fills the header of a response to `req` with
// the given code (opcode2) and total length `total` (header + content).
void rr_finish(uint8_t *pkt, const uint8_t *req, uint16_t code, uint16_t total);
// Build a request packet header (make_packet).
void rr_header(uint8_t *pkt, uint16_t start_code, uint16_t total, uint16_t seq,
               uint16_t op1, uint16_t op2);

// ARC paginated list (serialize_items + extract_start_index +
// paginate_make_response). The builder must be positioned at RR_HEADER_LEN
// (offsets are relative to the start of the packet, as in Inferno).
// `item` is called for source index i (from the start index on); it may
// append data to bb (strings, descriptors) and must write `item_size` bytes
// into `out`; returning false skips the source element (Rust `None`).
// Returns the response code (1, 0x8112 when more items exist, 0xFFFF for an
// invalid request in which case nothing is written).
typedef bool (*arc_item_fn)(bb_t *bb, size_t i, uint8_t *out, void *ctx);
uint16_t arc_paginate(bb_t *bb, const uint8_t *req_content, size_t req_len,
                      uint8_t space_items, size_t item_size, size_t n_source,
                      arc_item_fn item, void *ctx);
