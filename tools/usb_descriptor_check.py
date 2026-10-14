#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode and check the usb-inferno UAC2 configuration descriptor.

Sources (pick one):
  --sysfs            the attached device, via /sys/bus/usb/devices/*/descriptors
                     (Linux, no root needed; matches --vid/--pid)
  --elf FILE         the firmware image: reads symbol s_cfg_hs (needs pyelftools,
                     e.g. the ESP-IDF python env)
  --bin FILE         raw configuration descriptor bytes

Prints a readable dump and checks what docs/10-usb.md promises: IAD with 3
interfaces, internal fixed clock with read-only controls, N-ch terminals
(--ch, default 32: the firmware's UI_CHANNELS build setting)
without Feature Units, alt 0 / alt 1 per AS interface, PCM 4-byte/24-bit,
async iso data EPs of 7 x N x 4 bytes (896 at 32 ch) at bInterval 1, a
4-byte feedback EP.
Exit status 0 = all checks passed.
"""
import argparse
import glob
import os
import struct
import sys

CH = 32                 # set from --ch in main()
MPS = CH * 4 * 7

SUB_AC = {1: "HEADER", 2: "INPUT_TERMINAL", 3: "OUTPUT_TERMINAL", 4: "MIXER_UNIT",
          5: "SELECTOR_UNIT", 6: "FEATURE_UNIT", 10: "CLOCK_SOURCE", 11: "CLOCK_SELECTOR",
          12: "CLOCK_MULTIPLIER", 13: "SAMPLE_RATE_CONVERTER"}
SUB_AS = {1: "AS_GENERAL", 2: "FORMAT_TYPE"}


def from_sysfs(vid, pid):
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            v = int(open(os.path.join(d, "idVendor")).read(), 16)
            p = int(open(os.path.join(d, "idProduct")).read(), 16)
        except OSError:
            continue
        if v == vid and p == pid:
            raw = open(os.path.join(d, "descriptors"), "rb").read()
            speed = open(os.path.join(d, "speed")).read().strip()
            print(f"device {d}  speed {speed} Mbit/s")
            return raw[raw[0]:]           # skip the 18-byte device descriptor
    sys.exit(f"no device {vid:04x}:{pid:04x} in /sys/bus/usb/devices")


def from_elf(path):
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        sys.exit("pyelftools missing: run with the ESP-IDF python env")
    f = ELFFile(open(path, "rb"))
    symtab = f.get_section_by_name(".symtab")
    if symtab is None:
        sys.exit(f"{path} has no .symtab (stripped?)")
    sym = symtab.get_symbol_by_name("s_cfg_hs")
    if not sym:
        sys.exit("symbol s_cfg_hs not found")
    addr, size = sym[0]["st_value"], sym[0]["st_size"]
    for sec in f.iter_sections():
        a = sec["sh_addr"]
        if sec["sh_type"] != "SHT_NOBITS" and a <= addr < a + sec["sh_size"]:
            return sec.data()[addr - a:addr - a + size]
    sys.exit("s_cfg_hs not in a loadable section")


class Check:
    def __init__(self):
        self.fails = 0

    def __call__(self, cond, msg):
        if not cond:
            self.fails += 1
            print(f"  FAIL: {msg}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--sysfs", action="store_true")
    src.add_argument("--elf")
    src.add_argument("--bin")
    ap.add_argument("--vid", type=lambda x: int(x, 16), default=0x303A)
    ap.add_argument("--pid", type=lambda x: int(x, 16), default=0x8A32)
    ap.add_argument("--ch", type=int, default=32, choices=(2, 4, 8, 16, 32),
                    help="channels per direction of the build (UI_CHANNELS)")
    a = ap.parse_args()
    global CH, MPS
    CH = a.ch
    MPS = CH * 4 * 7

    if a.sysfs:
        d = from_sysfs(a.vid, a.pid)
    elif a.elf:
        d = from_elf(a.elf)
    else:
        d = open(a.bin, "rb").read()

    ck = Check()
    if len(d) < 9:
        sys.exit("usb_descriptor_check: config descriptor shorter than 9 bytes")
    ck(d[1] == 2, "first descriptor is not a configuration")
    total = struct.unpack_from("<H", d, 2)[0]
    ck(total == len(d) or a.sysfs, f"wTotalLength {total} != {len(d)}")
    d = d[:total]
    if len(d) < 9:
        sys.exit("usb_descriptor_check: truncated configuration")
    print(f"configuration: wTotalLength {total}, bNumInterfaces {d[4]}, bmAttributes 0x{d[7]:02x}, "
          f"MaxPower {d[8] * 2} mA")

    itf = alt = None
    seen = {"iad": 0, "clock": [], "it": [], "ot": [], "fu": 0, "ep": [], "fmt": [], "asg": []}
    alts = {}
    i = d[0]
    while i < len(d):
        if i + 2 > len(d):
            ck(False, f"truncated descriptor header at {i}")
            break
        ln, ty = d[i], d[i + 1]
        b = d[i:i + ln]
        if ln < 2:
            ck(False, f"zero-length descriptor at {i}")
            break
        if i + ln > len(d):
            ck(False, f"descriptor at {i} (type 0x{ty:02x}) overruns the buffer ({ln} B)")
            break
        if ty == 0x0B:
            seen["iad"] += 1
            print(f"IAD first {b[2]} count {b[3]} class {b[4]:02x}/{b[5]:02x}/{b[6]:02x} iFunction {b[7]}")
            ck(b[2] == 0 and b[3] == 3 and b[4] == 1 and b[6] == 0x20, "IAD fields")
        elif ty == 0x04:
            itf, alt = b[2], b[3]
            alts.setdefault(itf, []).append((alt, b[4]))
            print(f"interface {itf} alt {alt}: {b[4]} EP, class {b[5]:02x}/{b[6]:02x}/{b[7]:02x}")
            ck(b[5] == 1 and b[7] == 0x20, "interface not AUDIO / IP_VERSION_02_00")
        elif ty == 0x24 and itf == 0:
            st = b[2]
            name = SUB_AC.get(st, f"0x{st:02x}")
            if st == 1:
                print(f"  AC HEADER bcdADC {b[4]:x}.{b[3]:02x} category 0x{b[5]:02x} "
                      f"wTotalLength {struct.unpack_from('<H', b, 6)[0]}")
            elif st == 10:
                print(f"  CLOCK_SOURCE id {b[3]} bmAttributes 0x{b[4]:02x} bmControls 0x{b[5]:02x}")
                seen["clock"].append(b[3])
                ck(b[4] & 3 == 1, "clock is not internal fixed")
                ck(b[5] & 3 == 1, "frequency control not read-only")
                ck((b[5] >> 2) & 3 in (0, 1), "validity control writable")
            elif st == 2:
                tt, nch, cfg, names = struct.unpack_from("<H", b, 4)[0], b[8], \
                    struct.unpack_from("<I", b, 9)[0], b[13]
                print(f"  INPUT_TERMINAL id {b[3]} type 0x{tt:04x} clock {b[7]} ch {nch} "
                      f"cfg 0x{cfg:x} iChannelNames {names}")
                seen["it"].append((b[3], tt, nch, names))
                ck(nch == CH and cfg == 0, "IT channel cluster")
            elif st == 3:
                tt = struct.unpack_from("<H", b, 4)[0]
                print(f"  OUTPUT_TERMINAL id {b[3]} type 0x{tt:04x} source {b[7]} clock {b[8]}")
                seen["ot"].append((b[3], tt, b[7]))
            else:
                print(f"  {name}")
                if st == 6:
                    seen["fu"] += 1
        elif ty == 0x24:
            st = b[2]
            if st == 1:
                fmts, nch, names = struct.unpack_from("<I", b, 6)[0], b[10], b[15]
                print(f"  AS_GENERAL link {b[3]} format type {b[5]} bmFormats 0x{fmts:x} ch {nch} "
                      f"iChannelNames {names}")
                seen["asg"].append((itf, b[3], nch, names))
                ck(fmts == 1 and nch == CH, f"AS_GENERAL not PCM/{CH}ch")
            elif st == 2:
                print(f"  FORMAT_TYPE_I subslot {b[4]} bits {b[5]}")
                seen["fmt"].append((itf, b[4], b[5]))
                ck(b[4] == 4 and b[5] == 24, "format not 4-byte / 24-bit")
        elif ty == 0x05:
            if len(b) < 7:
                ck(False, f"endpoint descriptor at {i} shorter than 7 bytes")
                break
            addr, attr, mps, ival = b[2], b[3], struct.unpack_from("<H", b, 4)[0], b[6]
            sync = ["none", "async", "adaptive", "sync"][(attr >> 2) & 3]
            usage = ["data", "feedback", "implicit fb", "?"][(attr >> 4) & 3]
            span = 1 << (ival - 1) if ival else 0
            print(f"  EP 0x{addr:02x} iso={attr & 3 == 1} sync={sync} usage={usage} "
                  f"wMaxPacketSize {mps & 0x7FF}(+{mps >> 11}) bInterval {ival} "
                  f"(= {span} uframes)")
            seen["ep"].append((itf, addr, attr, mps, ival))
        elif ty == 0x25:
            print(f"  CS_ENDPOINT attr 0x{b[3]:02x} controls 0x{b[4]:02x} lockdelay {b[5]}/{b[6]}")
        i += ln

    ck(seen["iad"] == 1, "exactly one IAD")
    ck(len(seen["clock"]) == 1, "exactly one clock source")
    ck(seen["fu"] == 0, "Feature Unit present (device must be bit-transparent)")
    ck(sorted(alts.get(1, [])) == [(0, 0), (1, 2)], "itf 1 alts (0: 0 EP, 1: 2 EP)")
    ck(sorted(alts.get(2, [])) == [(0, 0), (1, 1)], "itf 2 alts (0: 0 EP, 1: 1 EP)")
    eps = {e[1]: e for e in seen["ep"]}
    ck(0x01 in eps and eps[0x01][2] == 0x05 and eps[0x01][3] == MPS and eps[0x01][4] == 1,
       f"OUT EP 0x01 iso async data {MPS} B bInterval 1")
    ck(0x82 in eps and eps[0x82][2] == 0x05 and eps[0x82][3] == MPS and eps[0x82][4] == 1,
       f"IN EP 0x82 iso async data {MPS} B bInterval 1")
    ck(0x81 in eps and eps[0x81][2] == 0x11 and eps[0x81][3] == 4,
       "feedback EP 0x81 iso feedback 4 B")
    names = sorted(t[3] for t in seen["it"])
    ck(len(names) == 2 and len(names) == 2 and names[1] - names[0] == CH,
       "channel-name string blocks" if len(names) == 2 else
       f"channel-name string blocks ({len(names)} input terminals found)")
    print("OK" if ck.fails == 0 else f"{ck.fails} check(s) FAILED")
    if a.sysfs:
        print("strings: lsusb -v -d %04x:%04x | less   (channel names are the iChannelNames indices)"
              % (a.vid, a.pid))
    sys.exit(1 if ck.fails else 0)


if __name__ == "__main__":
    main()
