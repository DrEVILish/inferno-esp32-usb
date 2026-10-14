#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Probe an Inferno-compatible AoIP device over ARC (UDP 4440) and CMC (8800).

Message layouts follow Inferno (protocol/req_resp.rs, protocol/proto_arc.rs).

  aoip_probe.py <ip>                      everything below
  aoip_probe.py <ip> info|tx|rx|flows     one section
  aoip_probe.py <ip> subscribe <rx#> <txchan> <txdevice>
  aoip_probe.py <ip> unsubscribe <rx#>
  aoip_probe.py <ip> rename-rx <rx#> <name>  |  rename-tx <tx#> <name>
  aoip_probe.py <ip> rename-device <name>    (0x1001, network-audio-controller style)
"""
import socket
import struct
import sys

ARC, CMC = 4440, 8800
_seq = 0x100


def request(ip, port, op, content=b"", start=0x27FF, timeout=2.0):
    global _seq
    _seq = (_seq + 1) & 0xFFFF
    pkt = struct.pack(">HHHHH", start, 10 + len(content), _seq, op, 0) + content
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(pkt, (ip, port))
        while True:
            data, _ = s.recvfrom(4096)
            if len(data) >= 10 and struct.unpack(">H", data[4:6])[0] == _seq:
                code = struct.unpack(">H", data[8:10])[0]
                return code, data
    except socket.timeout:
        return None, b""
    finally:
        s.close()


def cstr(pkt, off):
    if off == 0 or off >= len(pkt):
        return None
    end = pkt.find(b"\0", off)
    return pkt[off:end if end >= 0 else len(pkt)].decode("utf-8", "replace")


def paged(ip, op, item_size):
    """Yield (packet, item_offset) for every item of a paginated ARC list."""
    start = 1
    while True:
        code, pkt = request(ip, ARC, op, struct.pack(">HH", 0, start))
        if code is None:
            print(f"  no reply to 0x{op:04x}")
            return
        if code not in (1, 0x8112) or len(pkt) < 12:
            print(f"  0x{op:04x}: code 0x{code:04x}")
            return
        count = pkt[11]
        for i in range(count):
            yield pkt, 12 + i * item_size
        if code != 0x8112 or count == 0:
            return
        start += count


def info(ip):
    code, pkt = request(ip, CMC, 0x1001)
    if code == 1 and len(pkt) >= 32:
        c = pkt[10:]
        pid, devid, _, _, addr, port = struct.unpack(">H8sHH4sH", c[:20])
        print(f"CMC: process {pid} id {devid.hex()} ip {socket.inet_ntoa(addr)} info port {port}")
    else:
        print("CMC: no reply")
    code, pkt = request(ip, ARC, 0x1002)
    if code is not None:
        print(f"name (0x1002): {cstr(pkt, 10)}")
    code, pkt = request(ip, ARC, 0x1003)
    if code == 1 and len(pkt) >= 48:
        h = struct.unpack(">19H", pkt[10:48])
        print(f"names (0x1003): friendly {cstr(pkt, h[6])!r} factory {cstr(pkt, h[7])!r} "
              f"board {cstr(pkt, h[3])!r} revision {cstr(pkt, h[4])!r}")
    code, pkt = request(ip, ARC, 0x1000)
    if code == 1 and len(pkt) >= 44:
        f = struct.unpack(">BBHHHHHHHHHH", pkt[10:32])
        print(f"counts (0x1000): flags 0x{f[1]:02x} tx {f[2]} rx {f[3]} max ch/flow {f[5]} "
              f"max tx flows {f[7]} max rx flows {f[8]}")


def common(pkt, off):
    if not off:
        return ""
    rate, _, _, bits = struct.unpack(">IBBH", pkt[off:off + 8])
    return f"{rate} Hz {bits} bit"


def tx(ip):
    print("TX channels (0x2000 factory / 0x2010 friendly):")
    friendly = {}
    for pkt, o in paged(ip, 0x2010, 6):
        cid, _, noff = struct.unpack(">HHH", pkt[o:o + 6])
        friendly[cid] = cstr(pkt, noff)
    for pkt, o in paged(ip, 0x2000, 8):
        cid, _, coff, noff = struct.unpack(">HHHH", pkt[o:o + 8])
        print(f"  {cid:3d} {cstr(pkt, noff):>8}  friendly {friendly.get(cid)!r:20} {common(pkt, coff)}")


STATUS = {0: "-", 1: "unresolved", 8: "in progress", 0x14: "too many tx flows", 0x15: "tx fail",
          0x01010009: "RECEIVING (unicast)", 0x0101000A: "RECEIVING (multicast)"}


def rx(ip):
    print("RX channels (0x3000):")
    for pkt, o in paged(ip, 0x3000, 20):
        cid, _, coff, tco, tho, fo, st, _ = struct.unpack(">HHHHHHII", pkt[o:o + 20])
        src = f"{cstr(pkt, tco)}@{cstr(pkt, tho)}" if tco else ""
        print(f"  {cid:3d} {cstr(pkt, fo)!r:12} <- {src:32} {STATUS.get(st, hex(st))}")


def flows(ip):
    print("TX flows (0x2200):")
    for pkt, o in paged(ip, 0x2200, 2):
        (main,) = struct.unpack(">H", pkt[o:o + 2])
        fid, ftype, rate, _, bits, _, n, rsd = struct.unpack(">HHIHHHHH", pkt[main:main + 18])
        chans = struct.unpack(f">{n}H", pkt[main + 18:main + 18 + 2 * n])
        _, port, addr = struct.unpack(">HH4s", pkt[rsd:rsd + 8])
        (names,) = struct.unpack(">H", pkt[main + 18 + 2 * n:main + 20 + 2 * n])
        nd = struct.unpack(">6H", pkt[names:names + 12])
        kind = "multicast" if ftype == 2 else "unicast"
        print(f"  flow {fid} {kind} -> {socket.inet_ntoa(addr)}:{port} {rate} Hz {bits} bit ch {list(chans)} "
              f"rx {cstr(pkt, nd[2])!r} flow {cstr(pkt, nd[3])!r} local {cstr(pkt, nd[5])!r}")
    print("RX flows (0x3200):")
    for pkt, o in paged(ip, 0x3200, 2):
        (main,) = struct.unpack(">H", pkt[o:o + 2])
        fid, _, rate, _, bits, _, n, words, rsd = struct.unpack(">HHIHHHHHH", pkt[main:main + 20])
        masks = struct.unpack(f">{n}H", pkt[main + 20:main + 20 + 2 * n])
        (d2,) = struct.unpack(">H", pkt[main + 20 + 2 * n:main + 22 + 2 * n])
        _, port, addr = struct.unpack(">HH4s", pkt[rsd:rsd + 8])
        lat = struct.unpack(">I", pkt[d2 + 8:d2 + 12])[0]
        mapping = []
        for m in masks:
            bits_set = 0
            for w in range(words):
                bits_set |= struct.unpack(">H", pkt[m + 2 * w:m + 2 * w + 2])[0] << (16 * w)
            mapping.append([i + 1 for i in range(16 * words) if bits_set >> i & 1])
        print(f"  flow {fid} port {port} at {socket.inet_ntoa(addr)} {rate} Hz latency {lat / 1e6:.2f} ms "
              f"channel -> local {mapping}")


def subscribe(ip, rxn, txchan, txdev):
    base = 10 + 2 + 6
    content = struct.pack(">BBHHH", 0, 1, rxn, base, base + len(txchan) + 1)
    content += txchan.encode() + b"\0" + txdev.encode() + b"\0"
    print("subscribe:", hex(request(ip, ARC, 0x3010, content)[0] or 0))


def unsubscribe(ip, rxn):
    content = struct.pack(">BBHHH", 0, 1, rxn, 0, 0)
    print("unsubscribe:", hex(request(ip, ARC, 0x3010, content)[0] or 0))


def rename(ip, which, n, name):
    if which == "tx":
        base = 10 + 2 + 6
        content = struct.pack(">BBHHH", 0, 1, 0, n, base) + name.encode() + b"\0"
        op = 0x2013
    else:
        base = 10 + 2 + 4
        content = struct.pack(">BBHH", 0, 1, n, base) + name.encode() + b"\0"
        op = 0x3001
    print(f"rename {which}:", hex(request(ip, ARC, op, content)[0] or 0))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ip, cmd = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "all")
    a = sys.argv[3:]
    if cmd in ("all", "info"):
        info(ip)
    if cmd in ("all", "tx"):
        tx(ip)
    if cmd in ("all", "rx"):
        rx(ip)
    if cmd in ("all", "flows"):
        flows(ip)
    if cmd == "subscribe":
        subscribe(ip, int(a[0]), a[1], a[2])
    elif cmd == "unsubscribe":
        unsubscribe(ip, int(a[0]))
    elif cmd in ("rename-tx", "rename-rx"):
        rename(ip, cmd[-2:], int(a[0]), a[1])
    elif cmd == "rename-device":
        code, _ = request(ip, ARC, 0x1001, a[0].encode() + b"\0")
        print("rename device:", hex(code or 0))


if __name__ == "__main__":
    main()
