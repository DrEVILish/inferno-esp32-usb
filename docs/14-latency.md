# 14 — Latency budget

Status: 2026-10-13, derived from the code and the USB engine simulation.
**Not measured on hardware yet**; docs/21-latency-matrix.md is the test plan
and `tools/latency_matrix.py` the model behind its predictions.

Targets given: < 5 ms, then < 2 ms. Read as **< 2 ms one way through the
device in each direction**. A *round trip through a computer* cannot reach
2 ms: the host's audio buffering alone is ~1.5 ms or more (§4).

## 1. Latency profiles

One compile-time setting, `UI_LAT_PROFILE` (Kconfig "usb-inferno build
profile", `app_config.h`). The number is the round-trip target with a
computer in the loop. Values are the PROFILES table of
`tools/latency_matrix.py`.

| profile | AoIP latency `UI_NET_LATENCY_FRAMES` | fpp (`UI_FPP_DEFAULT` / max `UI_TX_FPP_MAX`) | USB OUT lead `UI_USB_OUT_LEAD_FRAMES` | USB IN margin | network |
|---|---|---|---|---|---|
| **10 (default)** | 48 = 1 ms | 16 / 32 | 48 = 1 ms | 3 | any AoIP network |
| 6 | 48 = 1 ms | 16 / 32 | 12 = 0.25 ms | 3 | any AoIP network |
| 4 | 24 = 0.5 ms | 8 / 8 | 12 | 3 | one switch, QoS |
| 2 | 12 = 0.25 ms | 4 / 4 | 12 | 3 | direct link or one QoS switch, nothing else on it |

What follows from the profile: the latency we advertise for our TX flows
(mDNS `latency_ns`), our RX playout point, the largest fpp we advertise and
request, the fpp of our multicast bundles, the USB OUT lead and the OUT
underrun thresholds.

**Why the default is 10, not 6.** Profiles 10 and 6 differ only in the USB
OUT lead (1 ms vs 0.25 ms); the AoIP side is identical. The 12-frame lead
passes the simulation with 2 frames (42 µs) to spare under the modelled
worst case (§3), but that model's ISR latency is a guess until bench test
B2 measures `lat_usb_out_headroom_min`. 1 ms of lead costs 0.75 ms one way
and is robust to anything short of a 1 ms USB ISR stall. Switch the
default to 6 once B2 shows `lat_usb_out_headroom_min` ≥ 4 frames over a
long run with 12.

## 2. Per-direction budget through the device

All in frames at 48 kHz (1 frame = 20.8 µs). L = AoIP latency of the flow
(ours unless the transmitter demands more), lead = USB OUT lead, m = USB IN
margin = 3.

### Network → USB (AoIP RX → USB IN)

A sample with AoIP timestamp f:

1. arrives in a packet some time before f + L (else it is late and dropped:
   `aoip_rx_late`; `lat_rx_margin_min` shows how much earlier it came);
2. is written to ring slot f + (L_flow − L) (L_flow − L ≤ 256 frames: the
   ring is 512 frames);
3. is read into an IN packet at the first SOF whose media frame is
   ≥ f + L_flow + m, i.e. at f + L + m … f + L + m + 6 depending on where
   it falls in the microframe (`lat_usb_in_age_*`);
4. reaches the host in the next microframe (≤ 6 frames).

**one way = L + m + (0…6) + (≤ 6) = L + 3 … L + 15 frames.**

### USB → network (USB OUT → AoIP TX)

A sample the host sends in microframe k at position p (0…6) of the packet:

1. is written to ring index F(SOF_k) + lead + p (the feedback loop holds
   the lead; `lat_usb_out_headroom_*` shows what is actually achieved at
   the write);
2. goes out in the TX packet whose timestamp covers that index, sent when
   the media clock passes timestamp + fpp;
3. is played by the receiver at index + L_rx (its latency for our flow, ≥
   our advertised L).

**one way = lead + p + L = lead + L … lead + L + 6 frames.**

**fpp is not added.** A TX packet's timestamp is the media time of its
first sample and the receiver plays it at timestamp + latency, so
packetisation, our send lag and network transit all happen *inside* L. They
only have to fit: fpp + send lag (`lat_tx_send_lag_max`) + transit + the
receiver's own margin < L. At profile 2: 4 + ~2 + ~1 < 12. (The first
version of `latency_matrix.py` added fpp; corrected.) The same holds for
RX: the transmitter's fpp is inside L_flow; that is why we request at most
`UI_TX_FPP_MAX`.

### Result

| profile | net → USB | USB → net | device round trip (mode B is 2·L) |
|---|---|---|---|
| 10 | 51…63 = **1.06…1.31 ms** | 96…102 = **2.0…2.1 ms** | 3.1…3.4 ms |
| 6 | 51…63 = **1.06…1.31 ms** | 60…66 = **1.25…1.38 ms** | 2.3…2.7 ms |
| 4 | 27…39 = **0.56…0.81 ms** | 36…42 = **0.75…0.88 ms** | 1.3…1.7 ms |
| 2 | 15…27 = **0.31…0.56 ms** | 24…30 = **0.50…0.63 ms** | 0.8…1.2 ms |

**< 2 ms one way in both directions: profiles 6, 4 and 2.** Profile 10
meets it on RX only (USB → net 2.0–2.1 ms because of the 1 ms lead).

## 3. USB OUT lead: how small is robust

The lead has to cover the time between the start of the microframe in which
the host sends a packet and the moment the USB ISR has copied it into the
ring: up to one microframe (6 frames) for the transaction, plus interrupt
latency. The AoIP packetizer needs frame f no earlier than f + 1, so a
write is in time iff its first frame ≥ the real media clock at the write
(headroom ≥ 0).

`tools/usb_engine_sim.c` now measures exactly that headroom (min over the
run, count of late writes) in all scenarios. Two scenarios were added for
this: the OUT transaction completing anywhere in the microframe (0–125 µs)
plus 2–10 µs ISR latency and a 60 µs spike on 1 % of packets, alone and
with a lost packet every 3 s. Sweep (`LEAD=<n> tools/usb_engine_sim.sh 6`):

| lead (frames) | min headroom, late completion | late writes | verdict |
|---|---|---|---|
| 8 | −2 | yes | fails |
| 10 | 0 | 0 | passes, no margin |
| **12** | **2 (42 µs)** | 0 | **passes: smallest with margin** |
| 16 | 6 (125 µs) | 0 | passes |
| 24 | 14 | 0 | passes |
| 48 | 38 | 0 | passes |

**Jitter assumption behind lead 12:** the USB OUT completion interrupt runs
within ~165 µs of the start of the microframe in which the host sent the
packet (125 µs bus + ≤ 40 µs latency incl. occasional spikes). That needs
A2 (no flash stalls for the USB ISR) and no critical section longer than
~40 µs on the USB core. A 1 ms USB ISR stall is *not* absorbed by any lead
≤ 1 ms (simulated: lead 48 dips to −1 frame and re-anchors; it needs
≥ 1 ms + one microframe); with lead 12 it costs one OUT re-anchor and
~1 ms of TX silence. If B2 shows `lat_usb_out_headroom_min` < 2, use 16.

The phase loop holds the average lead within ±1 frame of the target in
every scenario (unchanged from docs/10-usb.md §3).

## 4. Host side (not the device; estimates until mode C measures them)

| host | what adds latency | typical minimum |
|---|---|---|
| Linux, JACK/ALSA (`snd-usb-audio`) | capture: 1 period; playback: `-n` periods (2); plus URB queueing in the driver (≈ 0.5 ms each way with low-latency playback mode, kernel ≥ 5.15) | `-p 16 -n 2`: 3 × 0.33 + 2 × 0.5 ≈ 2.0 ms; `-p 8`: ≈ 1.5 ms |
| macOS CoreAudio (`AppleUSBAudio`) | I/O buffer (≥ 14–32 frames per direction) + the driver's safety offsets (device-reported, tens of frames) + its isochronous scheduling in ~1 ms chunks | ≈ 3–5 ms round trip at 32 frames |
| Windows 10/11 `usbaudio2.sys` | WASAPI shared: 10 ms engine period; exclusive / low-latency: ≥ ~2–3 ms per direction; ASIO needs a third-party driver | ≈ 5–10 ms round trip |

These are the numbers `HOST_DRV_FRAMES` (24 per direction, unverified) and
the period choices in `latency_matrix.py` stand for. Mode C of docs/21
(USB loopback build) measures the host + USB share directly.

## 5. Round trip

| profile | device (net → USB + USB → net) | host loop (model) | predicted RT |
|---|---|---|---|
| 10 | 3.2 ms | JACK 64 × 2: 5.0 ms | ≈ 8.2 ms |
| 6 | 2.4 ms | JACK 16 × 2: 2.0 ms | ≈ 4.4 ms |
| 4 | 1.4 ms | JACK 16 × 2: 2.0 ms | ≈ 3.4 ms |
| 2 | 0.9 ms | JACK 8 × 2: 1.5 ms | ≈ 2.4 ms |

Full table per channel count with packet rates: docs/21-latency-matrix.md.
**< 5 ms round trip: profile 6 (or lower) on a tuned Linux host.** **< 2 ms
round trip with a computer in the loop is not reachable**: even with a
0.25 ms AoIP latency the host's own buffering is ≈ 1.5 ms. Network to
network through the device alone (mode B) is 2 · L: 0.5 ms at profile 2.

## 6. Packet rate — the main risk of profiles 4 and 2

fpp 8 doubles, fpp 4 quadruples the packet rate:

| profile | our TX (4 × 8-ch flows) | RX from 8-ch flows | RX from 4-ch flows |
|---|---|---|---|
| 10 / 6 (fpp 16) | 12 k/s | 12 k/s | 24 k/s |
| 4 (fpp 8) | 24 k/s | 24 k/s | 48 k/s |
| 2 (fpp 4) | 48 k/s | 48 k/s | 96 k/s |

The per-frame cost on the P4 (IDF copies every RX frame out of the DMA
buffer and mallocs) is unmeasured; ~24 k/s is the working estimate of the
ceiling. Bench test B4 (A5 stress tool) decides whether 0.5 ms is viable at
32 × 32. Lower channel counts scale the rate down (docs/21). The RX DMA
ring is now 64 descriptors (A4): 2.7 ms at 24 k/s, 1.3 ms at 48 k/s.

## 7. Telemetry that proves it on the bench

UDP 7779 (`tools/net_stats.py`). Windowed values cover the time since the
previous request and print `none` without samples.

| key | meaning | expected |
|---|---|---|
| `fw_build`, `fw_elf_sha256`, `cfg_*` | image identity, profile, channels, L, fpp, lead, margin | matches the matrix name |
| `lat_usb_out_headroom_min/max` | OUT data first frame − real media clock at the write (achieved lead) | min ≥ 2 (lead 12), ≈ lead − 6…lead |
| `usb_out_lead_frames` | smoothed lead the feedback loop holds | lead ± 1 |
| `lat_usb_in_age_min/max` | real media clock − newest frame of an IN packet when built | L + 3 … L + 3 + 6 (+ ISR latency) |
| `lat_rx_margin_min`, `lat_rx_slot<i>_margin_min` | RX packet: ts + flow latency − arrival (frames to spare) | ≥ 0; `aoip_rx_late` counts < 0 |
| `lat_tx_send_lag_max` | packetizer: send time − (ts + fpp) | ≪ L − fpp (a few frames) |
| `aoip_rx_late`, `aoip_tx_underruns`, `ring_*` | deadline misses | 0 |
| `cpu0_idle_pct`, `cpu1_idle_pct` | idle share since the previous request | > 20 % |

Bench procedure: docs/20-bench-testing.md, "Latency".
