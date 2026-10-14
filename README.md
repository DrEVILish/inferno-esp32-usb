# inferno-esp32-usb

Fixed-function 32×32 audio bridge: Inferno-compatible AoIP on RJ45 ↔ USB Audio Class 2.0 on USB-C. One ESP32-P4, no settings, boots and runs.

- **Network side:** AoIP device speaking the Dante-compatible protocol as implemented by [Inferno](https://github.com/teodly/inferno). 32 transmit + 32 receive channels, 48 kHz, 24-bit.
- **USB side:** class-compliant USB Audio Class 2.0 device. 32 playback + 32 capture channels, 48 kHz, High-Speed asynchronous with explicit feedback.
- **Mapping (fixed 1:1):** USB playback N → network TX N, network RX N → USB capture N.
- **Operation:** no configuration UI. Boots on USB-C bus power and runs (PoE deferred).

Target SoC: **ESP32-P4** — the only ESP32 with High-Speed USB (32×32 needs it; the S3's Full-Speed port tops out around 2–4 channels) plus an Ethernet MAC with IEEE 1588 hardware timestamps.

> Status: firmware for milestones M1–M5 is written but **not yet run on hardware** (2026-10-13). Until the bench matrix (docs/20/21) has passed, USB enumeration/throughput, Ethernet/PTP behaviour and the real-time figures quoted in the docs are **claims, not measurements** — do not rely on them. Next steps are bench order, own board, and open questions in `docs/30-roadmap.md`. Not affiliated with or endorsed by any AoIP protocol vendor.

**Threat model:** this is a dissector of an unauthenticated Dante-class
protocol on a LAN. The control plane (ARC/CMC/flow-control, ports 4440/8800/4455)
and info multicast accept any host: basic sanity (destination checks,
rate limits, name validation) is enforced, and abuse is counted in telemetry
(`aoip_info_drops`, `ptp_hw_*`, …), but there is no authentication — as with
Dante/Inferno itself. A hostile host on the same segment can re-route RX
channels, rename every channel, create or stop TX flows, and keep the
control task busy; info multicast is bounded by a token bucket and
degraded-startup state is visible in telemetry (`aoip_start_degraded`).
Assign it a port on a managed switch with ACLs / IGMP snooping, never a
flat, hostile network; if hostile LANs are actually in scope, an
authentication layer would have to be added on top (protocol-divergent).

Dev board: **Waveshare ESP32-P4-ETH** (RJ45/IP101; HS USB OTG on its 4-pin header → USB-C pigtail).

## Repo layout

| path | what |
|---|---|
| `firmware/` | ESP-IDF v6.1 firmware (C). See table below |
| `docs/` | Design notes, bench procedure, roadmap. See index below |
| `hardware/` | Own rev-1 board: SKiDL schematic-as-code → KiCad PCB (placement done, not routed) |
| `tools/` | Build scripts, host test/probe tools, simulations, host test runner |
| `patches/` | `esp_eth-rx64.patch`: raises IDF v6.1 EMAC RX ring cap 30 → 64 |
| `third_party/` | **Reference clones, study only, never committed** (see `.gitignore`): `inferno-esp32-p4` (8-ch RX-only prior art) and `inferno-upstream` (protocol reference) |
| `CHANGELOG.md` | Change log of the post-import work (releases 0.1.0/0.1.1) |

## Firmware

`firmware/` (ESP-IDF v6.1, C, GPL-3.0-or-later):

| part | files | doc |
|---|---|---|
| shared rings, config, telemetry | `main/audio`, `main/app_config.h`, `main/sys` | `docs/01-architecture.md` |
| Ethernet fast path, 1588 media clock, PTPv1 follower | `main/net`, `main/clock` | `docs/11-net-clock.md` |
| USB Audio 2.0, 32×32 HS async, own class driver on TinyUSB | `main/usb` | `docs/10-usb.md` |
| AoIP (Inferno-compatible): mDNS, ARC, CMC, info, flows TX/RX, subscriptions | `main/aoip` | `docs/12-aoip.md` |

Design details: single 1588 time base slaved to the PTPv1 leader; media clock derived arithmetically (no ASRC, bit-transparent). Network TX is 4×8-channel flows; USB uses 896-byte packets (6+1 frames) with a 4-byte feedback endpoint every 1 ms. IDF's `esp_eth` is overridden locally (`firmware/components/esp_eth` + `patches/` + `tools/esp_eth_override.sh`) for a 64-descriptor RX ring and hold-not-flush DMA behavior.

## Docs index

| doc | contents |
|---|---|
| `docs/00-plan.md` | Goal, starting points, milestones M0–M8, risks, open questions |
| `docs/01-architecture.md` | System design: clock architecture, bandwidth budgets |
| `docs/10-usb.md` | UAC2 32×32 HS async driver, descriptors, feedback engine, sim results |
| `docs/11-net-clock.md` | EMAC fast path, 1588 media clock, PTPv1 follower + servo |
| `docs/12-aoip.md` | AoIP module: ports, flows, subscriptions, NVS persistence |
| `docs/13-review-2026-10-13.md` | Cross-module review fixes (IRAM, NVS, rings, latency profiles) |
| `docs/14-latency.md` | Latency budget and profiles (10/6/4/2 ms) |
| `docs/15-profile.md` | Performance instrumentation, hot-path probes, gated optimization backlog |
| `docs/20-bench-testing.md` | Bench setup and test sequence (M1–M8) |
| `docs/21-latency-matrix.md` | Round-trip test matrix: 10/6/4/2 ms × 32/16/8/4/2 ch, modes A/B/C |
| `docs/30-roadmap.md` | Roadmap: no-HW phase, bench order, own board, release |
| `docs/40-board-rev1.md` | Own board rev 1: rationale, values, pins, power, risks |

## Hardware

`hardware/` holds the custom rev-1 board: USB-C (UAC2 HS, bus-powered) + RJ45 (10/100 RMII) on an ESP32-P4NRW32X, SKiDL schematic (`usb_inferno.py`) generating the KiCad netlist, BOM (`bom.csv`), and placed-but-unrouted 4-layer PCB. Full story in `hardware/README.md` and `docs/40-board-rev1.md`. Regenerate with `hardware/build.sh` (needs KiCad 9 + SKiDL venv); once human routing starts, stop regenerating the PCB and import netlist changes in pcbnew instead.

## Build

Prerequisites: ESP-IDF v6.1 with tools installed. `tools/idf-env.sh` sources it from `/opt/toolchain` — adjust the paths there if your IDF lives elsewhere.

```sh
tools/build.sh                         # production build (ends with check_iram.py)
tools/build-variant.sh USB_LOOPBACK    # bench variants: USB_LOOPBACK | NET_LOOPBACK | TONE
tools/build-matrix.sh                  # latency/channel test images (see docs/21-latency-matrix.md)
```

Every build ends with `tools/check_iram.py`: everything the USB interrupt can reach must be IRAM-resident (flash writes disable the cache). A failure fails the build.

Flash (example): `. tools/idf-env.sh && cd firmware && idf.py -p /dev/ttyACM0 flash`.

## Bench testing

Firmware written ≠ firmware proven. Start at `docs/20-bench-testing.md`: Waveshare board + managed switch with a Dante leader + Inferno on Linux, USB HS pigtail, telemetry over UDP (port 7779 via `tools/net_stats.py`).

| tool | purpose |
|---|---|
| `tools/build-variant.sh`, `tools/build-matrix.sh` | test images (loopback/tone, latency×channel matrix) |
| `tools/usb_loopback_test.py`, `tools/usb_descriptor_check.py` | M1: enumeration, 896 B packets, bit-exact OUT→IN |
| `tools/net_stats.py` | UDP telemetry receiver |
| `tools/aoip_probe.py`, `tools/aoip_pcap.py` | M3–M5: discovery/control probing, packet capture analysis |
| `tools/usb_engine_sim.sh`, `tools/ptp_servo_sim.c` | host-side simulations (feedback engine, servo) |
| `tools/latency_matrix.py` | latency model behind `docs/14` and `docs/21` |
| `tools/host_tests.sh` | run every host test (fpp select, perf windows, ts split, group bitmap, fork-comparable ARC/info replies) |

## Attribution and license

- Protocol reference: [teodly/inferno](https://github.com/teodly/inferno) (Rust, GPL-3.0+/AGPL-3.0+).
- Prior art consulted: `Nickster90s/Inferno-ESP32-P4` (8-ch receive-only; **no licence file — study material only**). It lives under `third_party/` and is excluded from version control by `.gitignore`, as is `third_party/inferno-upstream`.
- Firmware in this repo is written as GPL-3.0-or-later (see `tools/check_iram.py` SPDX header and `firmware/` notes). There is not yet a top-level `LICENSE` file; adding the GPL-3.0-or-later text is an open release task (Phase D in `docs/30-roadmap.md`).
- This project is not affiliated with or endorsed by any AoIP protocol vendor.
