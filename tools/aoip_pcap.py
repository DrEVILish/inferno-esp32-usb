#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode AoIP audio packets in a pcap (classic libpcap format).

Packet layout (Inferno flows_tx.rs): u8 2, u32 seconds, u32 sub-second
samples, then interleaved big-endian samples.

  aoip_pcap.py capture.pcap [--ch N] [--bits 24] [--rate 48000] [--port P]

For each UDP flow (src -> dst:port) prints packet count, frames per packet,
timestamp gaps / repeats and the per-channel peak level in dBFS.
"""
import argparse
import math
import struct
from collections import defaultdict


def packets(fn):
    d = open(fn, "rb").read()
    magic = struct.unpack("<I", d[:4])[0]
    e = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    lt = struct.unpack(e + "I", d[20:24])[0]
    off = 24
    while off + 16 <= len(d):
        ts, tu, cl, _ = struct.unpack(e + "IIII", d[off:off + 16])
        off += 16
        pkt = d[off:off + cl]
        off += cl
        ip = {1: pkt[14:], 113: pkt[16:], 276: pkt[20:]}.get(lt, pkt)
        if len(ip) < 28 or ip[0] >> 4 != 4 or ip[9] != 17:
            continue
        ihl = (ip[0] & 15) * 4
        sp, dp = struct.unpack(">HH", ip[ihl:ihl + 4])
        yield ts + tu / 1e6, ".".join(map(str, ip[12:16])), ".".join(map(str, ip[16:20])), sp, dp, ip[ihl + 8:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap")
    ap.add_argument("--ch", type=int, default=8)
    ap.add_argument("--bits", type=int, default=24)
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--port", type=int)
    a = ap.parse_args()
    bps = a.bits // 8
    stride = a.ch * bps
    flows = defaultdict(lambda: {"n": 0, "fpp": set(), "gaps": 0, "repeats": 0, "next": None,
                                 "peak": [0] * a.ch, "t0": None, "t1": None})
    for t, src, dst, sp, dp, pl in packets(a.pcap):
        if a.port and dp != a.port:
            continue
        if len(pl) <= 9 or pl[0] != 2 or (len(pl) - 9) % stride:
            continue
        f = flows[(src, sp, dst, dp)]
        sec, sub = struct.unpack(">II", pl[1:9])
        ts = sec * a.rate + sub
        n = (len(pl) - 9) // stride
        f["n"] += 1
        f["fpp"].add(n)
        f["t0"] = f["t0"] or t
        f["t1"] = t
        if f["next"] is not None:
            if ts > f["next"]:
                f["gaps"] += 1
            elif ts < f["next"]:
                f["repeats"] += 1
        f["next"] = ts + n
        for fi in range(n):
            base = 9 + fi * stride
            for c in range(a.ch):
                b = pl[base + c * bps:base + (c + 1) * bps]
                v = int.from_bytes(b, "big", signed=True)
                f["peak"][c] = max(f["peak"][c], abs(v))
    full = float(1 << (a.bits - 1))
    for (src, sp, dst, dp), f in flows.items():
        dur = (f["t1"] - f["t0"]) or 1e-9
        peaks = " ".join(f"{20 * math.log10(p / full):.1f}" if p else "-inf" for p in f["peak"])
        print(f"{src}:{sp} -> {dst}:{dp}  {f['n']} pkts ({f['n'] / dur:.0f}/s) fpp {sorted(f['fpp'])} "
              f"gaps {f['gaps']} repeats {f['repeats']}\n  peak dBFS: {peaks}")


if __name__ == "__main__":
    main()
