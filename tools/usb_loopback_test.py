#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""usb-inferno milestone M1: bit-exact USB round trip (Linux host).

Needs firmware built with CONFIG_USBINF_TEST_USB_LOOPBACK=y: capture channel N
returns playback channel N, delayed by a fixed number of frames.

The test plays a deterministic 32-channel pattern (every channel and every
frame different, 24-bit values left-justified in S32_LE, never zero), records
32 channels at the same time, finds the round-trip delay, and checks every
sample of every channel for equality. It reports per-channel results, the
delay, and any discontinuity (a lost or repeated block shifts the delay).

Backends (auto-selected, override with --backend):
  alsa         aplay + arecord on hw:CARD (alsa-utils; no Python packages)
  sounddevice  python-sounddevice + numpy, one full-duplex stream

    tools/usb_loopback_test.py                    # find the card, 10 s
    tools/usb_loopback_test.py -D hw:2,0 -t 60    # explicit device, 60 s

Exit status 0 = all channels bit-exact (32 by default, --ch for other builds).
"""
import argparse
import array
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter

RATE = 48000
CH = 32                 # --ch (the firmware build's UI_CHANNELS)
CARD_MATCH = re.compile(r"USB-Inferno|UI(32|16|08|04|02){2}|usb-inferno", re.I)

try:
    import numpy as np
except ImportError:
    np = None


M32 = 0xFFFFFFFF


def pattern_value(h, c):
    """24-bit hash of (frame, channel), never 0, left-justified in a signed int32."""
    x = ((h * CH + c) * 0x9E3779B1) & M32
    x ^= x >> 15
    x = (x * 0x85EBCA77) & M32
    x ^= x >> 13
    v = (x & 0xFFFFFF) or 1
    v <<= 8
    return v - (1 << 32) if v & 0x80000000 else v


def make_pattern(frames):
    """Interleaved int32 frames, S32_LE: 24-bit value in bits 31..8, low byte 0."""
    if np is not None:
        n = np.arange(frames * CH, dtype=np.uint64)
        x = (n * np.uint64(0x9E3779B1)) & np.uint64(M32)
        x ^= x >> np.uint64(15)
        x = (x * np.uint64(0x85EBCA77)) & np.uint64(M32)
        x ^= x >> np.uint64(13)
        v = x & np.uint64(0xFFFFFF)
        v[v == 0] = 1
        return (v.astype(np.uint32) << np.uint32(8)).view(np.int32)
    a = array.array("i", bytes(4 * frames * CH))
    for h in range(frames):
        base = h * CH
        for c in range(CH):
            a[base + c] = pattern_value(h, c)
    return a


def find_card():
    try:
        cards = open("/proc/asound/cards").read()
    except OSError:
        return None
    for line in cards.splitlines():
        m = re.match(r"\s*(\d+)\s+\[(.*?)\s*\]:\s*(.*)", line)
        if m and (CARD_MATCH.search(m.group(2)) or CARD_MATCH.search(m.group(3))):
            return f"hw:{m.group(1)},0"
    return None


def run_alsa(device, play_bytes, seconds):
    for tool in ("aplay", "arecord"):
        if not shutil.which(tool):
            raise RuntimeError(f"{tool} not found (install alsa-utils)")
    tmp = tempfile.mkdtemp(prefix="usbinf_")
    try:
        return capture_audio(device, play_bytes, seconds, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def capture_audio(device, play_bytes, seconds, tmp):
    pfile, cfile = os.path.join(tmp, "play.raw"), os.path.join(tmp, "cap.raw")
    with open(pfile, "wb") as f:
        f.write(play_bytes)
    fmt = ["-t", "raw", "-f", "S32_LE", "-c", str(CH), "-r", str(RATE)]
    rec_s = int(seconds + 3)
    rec = subprocess.Popen(["arecord", "-q", "-D", device, *fmt, "-d", str(rec_s), cfile],
                           stderr=subprocess.PIPE)
    time.sleep(1.0)                      # capture running before playback starts
    play = subprocess.run(["aplay", "-q", "-D", device, *fmt, pfile], stderr=subprocess.PIPE)
    _, rerr = rec.communicate()
    if play.returncode:
        raise RuntimeError("aplay: " + play.stderr.decode(errors="replace").strip())
    if rec.returncode:
        raise RuntimeError("arecord: " + rerr.decode(errors="replace").strip())
    return open(cfile, "rb").read()


def run_sounddevice(device, play, seconds):
    import sounddevice as sd
    if np is None:
        raise RuntimeError("sounddevice backend needs numpy")
    dev = device
    if dev is None:
        for i, d in enumerate(sd.query_devices()):
            if CARD_MATCH.search(d["name"]) and d["max_input_channels"] >= CH \
                    and d["max_output_channels"] >= CH:
                dev = i
                break
    if dev is None:
        raise RuntimeError(f"no sounddevice device with {CH} in / {CH} out matching USB-Inferno")
    lead = np.zeros((RATE, CH), dtype=np.int32)              # 1 s of silence first
    tail = np.zeros((2 * RATE, CH), dtype=np.int32)
    out = np.concatenate([lead, np.asarray(play).reshape(-1, CH), tail])
    rec = sd.playrec(out, samplerate=RATE, channels=CH, dtype="int32", device=dev, blocking=True)
    return rec.astype(np.int32).tobytes()


def analyse(play, cap_bytes, frames):
    cap = array.array("i")
    cap.frombytes(cap_bytes[: len(cap_bytes) // (4 * CH) * 4 * CH])
    ncap = len(cap) // CH
    print(f"played {frames} frames, captured {ncap} frames")
    if ncap == 0:
        return False

    # Index played frames by (ch0, ch1) -> frame number (48 bits: unique).
    index = {}
    for h in range(frames):
        index[(int(play[h * CH]), int(play[h * CH + 1]))] = h
    delays = Counter()
    per_frame = []
    silent = 0
    for j in range(ncap):
        k = (cap[j * CH], cap[j * CH + 1])
        h = index.get(k)
        if h is None:
            if k == (0, 0):
                silent += 1
            per_frame.append(None)
            continue
        delays[j - h] += 1
        per_frame.append(j - h)
    if not delays:
        print("FAIL: none of the played frames came back (is this the loopback build?)")
        return False
    delay, count = delays.most_common(1)[0]
    print(f"round-trip offset: capture index - play index = {delay} frames "
          f"({count} frames at this offset)")
    if sum(delays.values()) - count > 0:
        print(f"WARNING: {len(delays) - 1} other offsets seen (lost/repeated blocks?): "
              f"{dict(delays.most_common(6))}")

    # Segments of constant offset (a lost or repeated block starts a new one).
    segs = []                                   # [j_start, j_end, offset]
    for j, d in enumerate(per_frame):
        if d is None:
            continue
        if segs and segs[-1][2] == d and segs[-1][1] == j:
            segs[-1][1] = j + 1
        elif segs and segs[-1][2] == d:
            segs[-1][1] = j + 1                 # same offset across unmatched frames
        else:
            segs.append([j, j + 1, d])
    segs = [sg for sg in segs if sg[1] - sg[0] >= 8]   # ignore chance matches
    if len(segs) > 1:
        print(f"DISCONTINUITY: {len(segs) - 1} offset change(s):")
        for a_, b_ in zip(segs, segs[1:]):
            print(f"  at capture frame {b_[0]}: offset {a_[2]} -> {b_[2]} ({b_[2] - a_[2]:+d} frames)")
    span = sum(b - a for a, b, _ in segs)
    if span < RATE:
        print("FAIL: matched span shorter than 1 s")
        return False

    ok_all = len(segs) == 1
    print(f"comparing {span} frames ({span / RATE:.2f} s) in {len(segs)} segment(s), per channel:")
    for c in range(CH):
        nbad = bad24 = 0
        first = None
        for j0, j1, off in segs:
            if j0 - off < 0:
                # a segment whose playback offset precedes the capture start
                # cannot be compared (Python negative-slice would wrap to the
                # array tail and produce garbage "mismatches")
                print(f"  segment {j0}..{j1} offset {off}: not comparable (starts before capture)")
                continue
            got = cap[j0 * CH + c: j1 * CH + c: CH]
            want = play[(j0 - off) * CH + c: (j1 - off) * CH + c: CH]
            if np is not None:
                got_a = np.frombuffer(got.tobytes(), dtype=np.int32)
                want_a = np.frombuffer(want.tobytes(), dtype=np.int32)
                bad = np.nonzero(got_a != want_a)[0]
                if len(bad) and first is None:
                    first = j0 + int(bad[0])
                nbad += len(bad)
                bad24 += int(np.count_nonzero((got_a >> 8) != (want_a >> 8)))
            else:
                for i, (g, w) in enumerate(zip(got, want)):
                    if g != w:
                        nbad += 1
                        if first is None:
                            first = j0 + i
                        if (g >> 8) != (w >> 8):
                            bad24 += 1
        if nbad == 0:
            print(f"  ch {c + 1:2d}: OK (bit-exact)")
        else:
            ok_all = False
            extra = "" if bad24 == nbad else f", {nbad - bad24} differ only in the pad byte"
            print(f"  ch {c + 1:2d}: FAIL {nbad} samples differ{extra}; first at capture frame {first}")
    print(f"silent capture frames (outside the played span or dropped): {silent}")
    return ok_all


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-D", "--device", help="ALSA device (default: find USB-Inferno in /proc/asound/cards) "
                    "or sounddevice index/name")
    ap.add_argument("-t", "--seconds", type=float, default=10.0)
    ap.add_argument("--backend", choices=["auto", "alsa", "sounddevice"], default="auto")
    ap.add_argument("--ch", type=int, default=32, choices=(2, 4, 8, 16, 32),
                    help="channels per direction of the build (UI_CHANNELS)")
    a = ap.parse_args()
    global CH
    CH = a.ch

    frames = int(a.seconds * RATE)
    print(f"generating {a.seconds:.1f} s x {CH} ch pattern ...")
    play = make_pattern(frames)

    backend = a.backend
    if backend == "auto":
        backend = "alsa" if shutil.which("aplay") and shutil.which("arecord") else "sounddevice"
    try:
        if backend == "alsa":
            dev = a.device or find_card()
            if not dev:
                sys.exit("USB-Inferno not found in /proc/asound/cards; pass -D hw:N,0")
            print(f"backend alsa, device {dev}")
            play_bytes = play.tobytes()
            cap = run_alsa(dev, play_bytes, a.seconds)
        else:
            print("backend sounddevice")
            dev = a.device
            if dev is not None and dev.isdigit():
                dev = int(dev)
            cap = run_sounddevice(dev, play, a.seconds)
    except Exception as e:  # noqa: BLE001 - report any backend failure plainly
        sys.exit(f"capture/playback failed: {e}")

    play_arr = array.array("i")
    play_arr.frombytes(play.tobytes() if np is not None else play.tobytes())
    ok = analyse(play_arr, cap, frames)
    print("RESULT: PASS - all 32 channels bit-exact" if ok else "RESULT: FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
