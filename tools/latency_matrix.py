#!/usr/bin/env python3
"""Round-trip latency test matrix: 4 targets x 5 channel counts = 20 permutations.

Prints the parameter profile, the predicted budget, packet rates and link load
for every permutation (markdown with --md, CSV with --csv). The profiles here
are the source of truth for the LAT*/CH* build variants and docs/21.

Round trip (RT) is measured network-to-network with Inferno on the test PC:
  Inferno TX --net--> DUT RX --USB IN--> host loop --USB OUT--> DUT TX --net--> Inferno RX
RT = net_lat (sender->DUT playout) + usb_in + host + usb_lead + net_lat
fpp is NOT added (corrected 2026-10-13, docs/14-latency.md): a TX packet's
timestamp is the media time of its first sample and the receiver plays it at
timestamp + latency, so packetisation (fpp), send lag and network transit
all happen inside the AoIP latency; they only have to fit in it.
usb_in = UI_USB_IN_MARGIN_FRAMES (3) + one microframe of USB transit (6).
host = 1 period (capture) + 2 periods (playback) + 2 x USB host driver overhead
(JACK/ALSA convention; HOST_DRV_FRAMES is an UNVERIFIED estimate that mode C of
docs/21 measures). Inferno's own ALSA buffering is removed by the calibration run.
"""
import argparse, math

FS = 48000
TARGETS_MS = [10, 6, 4, 2]
CHANNELS = [32, 16, 8, 4, 2]
CH_PER_FLOW = 8            # Inferno TX/RX flows carry up to 8 channels
WIRE_OVERHEAD = 75         # preamble+IFG+Eth+IP+UDP+FCS (66) + AoIP audio header (9)
PPS_CEILING = 24000        # per direction; UNVERIFIED — set from bench test B4

HOST_DRV_FRAMES = 24       # per direction, host USB audio driver (~0.5 ms) — UNVERIFIED

# Per-target profile. Frames are at 48 kHz; period = host audio period (JACK -p).
PROFILES = {
    10: dict(net_lat=48, fpp=16, usb_lead=48, usb_in=9, period=64),  # 1 ms AoIP
    6:  dict(net_lat=48, fpp=16, usb_lead=12, usb_in=9, period=16),  # 1 ms AoIP
    4:  dict(net_lat=24, fpp=8,  usb_lead=12, usb_in=9, period=16),  # 0.5 ms AoIP
    2:  dict(net_lat=12, fpp=4,  usb_lead=12, usb_in=9, period=8),   # 0.25 ms AoIP, direct link
}

def host_frames(p):
    return 3 * p["period"] + 2 * HOST_DRV_FRAMES

def ms(frames):
    return frames * 1000.0 / FS

def row(target, ch):
    p = PROFILES[target]
    host = host_frames(p)
    dev_rt = p["net_lat"] + p["usb_in"] + p["usb_lead"] + p["net_lat"]
    rt = dev_rt + host
    net_only = 2 * p["net_lat"]                    # mode B: NET_LOOPBACK build (TX reads RX at ts - net_lat)
    flows = math.ceil(ch / CH_PER_FLOW)
    pps_flow = FS / p["fpp"]
    pps = flows * pps_flow
    chf = min(ch, CH_PER_FLOW)
    bytes_pkt = chf * 3 * p["fpp"] + WIRE_OVERHEAD
    mbps = flows * bytes_pkt * 8 * pps_flow / 1e6
    # 4-ch flows (some Dante hardware) double the RX packet rate
    pps_rx_worst = math.ceil(ch / 4) * pps_flow
    tgt = target * FS / 1000
    if pps > PPS_CEILING or mbps > 70 or rt > tgt:
        risk = "OVER"
    elif pps_rx_worst > PPS_CEILING or mbps > 50 or rt > 0.85 * tgt:
        risk = "AT RISK"
    else:
        risk = "OK"
    return dict(target_ms=target, ch=ch, net_lat_ms=ms(p["net_lat"]), fpp=p["fpp"],
                usb_lead_ms=ms(p["usb_lead"]), period=p["period"],
                host_ms=ms(host), device_ms=ms(dev_rt), net_only_ms=ms(net_only), rt_pred_ms=ms(rt), flows=flows, pps=int(pps),
                pps_rx_4ch=int(pps_rx_worst), mbps=mbps, risk=risk)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", action="store_true")
    ap.add_argument("--md", action="store_true")
    a = ap.parse_args()
    rows = [row(t, c) for t in TARGETS_MS for c in CHANNELS]
    cols = ["target_ms", "ch", "net_lat_ms", "fpp", "usb_lead_ms", "period",
            "host_ms", "device_ms", "net_only_ms", "rt_pred_ms", "flows", "pps", "pps_rx_4ch", "mbps", "risk"]
    fmt = lambda v: f"{v:.2f}" if isinstance(v, float) else str(v)
    if a.csv:
        print(",".join(cols))
        for r in rows:
            print(",".join(fmt(r[c]) for c in cols))
        return
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))
    for r in rows:
        print("| " + " | ".join(fmt(r[c]) for c in cols) + " |")

if __name__ == "__main__":
    main()
