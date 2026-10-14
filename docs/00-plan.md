# 00 — Plan

Goal: a USB-C ESP32 box with RJ45 (PoE deferred). It is a fixed 1:1 bridge
between 32 × 32 Inferno-compatible AoIP channels and 32 × 32 USB Audio
Class 2.0 channels. It has no configuration and boots straight into operation.

Design: `01-architecture.md`.

## Starting point

- **teodly/inferno** (Rust, Linux, GPL-3.0+/AGPL-3.0+) is the protocol
  reference. Its TX allows ≤ 8 channels per flow and ≤ 32 flows.
- **Nickster90s/Inferno-ESP32-P4** (C, ESP-IDF 5.5, last commit 2026-10-10)
  is an 8-ch **receive-only** AoIP → PCM1690 device on the Waveshare
  ESP32-P4-ETH. It works end to end: PTPv1 follower with HW stamps, servo,
  ARC/CMC/info/mDNS, subscriptions persisted, and audio decoded inside the EMAC
  RX path. **It has no licence file**, so it is study material only until the
  author grants one.
- **atomic14 article**: ESP32-S3 UAC via ESP-IDF. It is useful only as a
  warning: S3 is Full-Speed, speaker playback crackled, and the macOS/Windows
  descriptor toggle conflicted.

## Status

Firmware written for M1–M5 (2026-10-12), not yet run on hardware. Next steps: `30-roadmap.md`.

## Milestones

| # | milestone | done when |
|---|---|---|
| M0 | Bench + toolchain | IDF builds and flashes a P4 board. Test net: a Dante leader device, Dante Controller, Inferno on Linux, a capture port |
| M1 | **USB 32×32 alone** | UAC2 HS async enumerates on macOS, Linux and Windows with 32 in / 32 out at 48 kHz. Loopback OUT→IN is bit-exact. Feedback runs from a free-running 1588 clock |
| M2 | Network clock | EMAC 1588 time slaved to the PTPv1 leader, typically < ±1 µs, < 25 s to lock, telemetry over UDP |
| M3 | Discovery / control | Shows in the controller as a 32×32 device: names, ARC, CMC, info, heartbeat, mDNS |
| M4 | RX path | Subscriptions to 32 RX channels from several transmitters, persisted. Audio reaches USB IN with 0 late packets |
| M5 | TX path | Flow-control server plus multicast. 4 × 8-ch flows. USB OUT reaches Inferno/Dante receivers |
| M6 | Clock bridge closed | Feedback derived from the media clock. 24 h run: 0 USB under/overruns, 0 network late packets, bit-exact round trip via a loopback device |
| M7 | Robustness | Power-on to audio, link flap, leader change, transmitter reboot, host sleep/unplug, watchdog, firmware-update path |
| M8 | Own PCB | USB-C HS (bus-powered), RJ45, enclosure; PoE optional later. Phase-A firmware runs unchanged apart from pins |

M1 and M2 do not depend on each other and can run in parallel. M1 is first
because the USB side carries the least existing knowledge.

## Risks

| risk | mitigation |
|---|---|
| Packet rate (12–24 k pps each way) vs. CPU | decode/encode in the EMAC path (no lwIP), raw TX frames, fpp ≥ 16; measure in M4/M5 |
| 100 Mbit link: bursts from other devices (P4 port known issue 3) | QoS on the switch (DSCP as Dante), cap unicast TX fan-out |
| TinyUSB DWC2 HS isochronous at 8 k transfers/s each way on P4 | M1 tests this first. Fallback is to drive the DWC2 directly for the two iso EPs |
| Windows in-box UAC2 at 32 channels / async feedback | test in M1. Max packet size must include the +1 frame (the esp_device_uac bug) |
| Interop with real Dante hardware (4-ch unicast flows, multicast) | test against Dante hardware and Inferno; compare captures |
| 96 kHz 32×32 impossible on 10/100 EMAC | decision: 48 kHz fixed (Q1) |
| Protocol is reverse-engineered | Bench/research use. Not affiliated with the protocol vendor |

## Open questions (answers change the design)

- **Q1 Sample rate.** 48 kHz fixed? 96 kHz × 32×32 needs ~100 Mbit/s, which is
  more than the P4's 10/100 MAC carries. *Default: 48 kHz only.*
- **Q2 "Static".** Should the controller still be able to patch RX
  subscriptions, rename the device/channels and set latency (persisted on the
  box)? Or should everything be hard-wired, with e.g. auto-subscribe to a fixed
  transmitter? *Default: normal controller patching persisted on the box, with
  no settings on the box itself; fixed 48 kHz and 1 ms.*
- **Q3 Clock leader.** Must the box become PTP leader when it is alone with
  other followers (e.g. USB → one AoIP speaker)? *Default: follower only;
  free-run when no leader.*
- ~~Q4 Hardware path~~ **Decided 2026-10-12:** ignore PoE for now. Dev board
  is the Espressif ESP32-P4X-Function-EV-Board (USB-C on the HS OTG PHY,
  RJ45/IP101), see `01-architecture.md` §8.
- **Q5 Hosts.** Which host OSes matter (macOS / Windows / Linux) and in what
  order? Is Windows ASIO needed?
- **Q6 Licence.** What licence should this firmware use (GPL-3.0+ to match
  Inferno?), and should we ask Nickster90s to licence the P4 port so it can be
  reused directly?
- **Q7 Firmware updates.** UART header only, USB DFU, or network OTA?
- ~~Q8~~ **Decided:** the project lives in `/opt/usb-inferno`.
