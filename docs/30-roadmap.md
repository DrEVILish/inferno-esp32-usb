# 30 — Roadmap after first firmware (2026-10-12)

State: the full firmware is on `main` (e2b036c). The production build and the
3 test variants build clean, and the host simulations pass (USB clock engine,
PTP servo). **It has not yet run on hardware.** This plan orders the remaining
work so the things that could force a redesign are proven first.

## Phase A — work that needs no hardware (can start now)

| # | item | why | size |
|---|---|---|---|
| status 2026-10-13 | A1–A4 **done** on `feat/harden` (docs/13-review-2026-10-13.md); latency budget and profiles (docs/14-latency.md); build-time channel count and test matrix (docs/21, `tools/build-matrix.sh`) | | |
| A1 | **done** — **Cross-module review** of the merged code: ring contracts, ISR/IRAM safety, core and priority layout, init order, error paths | three agents wrote against stubs; the integration bugs live at the seams | M |
| A2 | **done** (IRAM path proven by `tools/check_iram.py` on every build; confirm in B9) — **USB ISR safe during flash writes.** Allocate the DWC2 interrupt with `ESP_INTR_FLAG_IRAM`, put TinyUSB `dcd_dwc2`/`usbd` and `usb/` ISR code in IRAM with a linker fragment, and add a build check that walks the map file so nothing reachable from the ISR sits in flash | an NVS write (patch or rename) currently stalls USB → audible re-anchor | M |
| A3 | **done** — **NVS write policy.** Coalesce controller changes and write after 2 s of quiet. Only write when something changed | fewer stalls; also flash wear | S |
| A4 | **done** (64, local override + patch) — **EMAC RX ring > 30.** Override the `esp_eth` component locally (copy plus Kconfig range patch, 64 RX descriptors), or drain the DMA faster | 30 descriptors ≈ 1.2 ms at 24 k frames/s | S–M |
| A5 | **Packet-rate stress tool** (C, Linux): sends N flows × fpp in the AoIP audio format at 48 kHz with correct timestamps, plus bursts. The device counters show what is dropped where | measures the CPU/descriptor ceiling on day 1 of hardware instead of guessing | S |
| A6 | **PTPv1 leader for the bench** without AoIP hardware: a small Linux PTPv1 master, or check whether an existing one (ptpd 1.x) works | lets M2–M5 run with Inferno on Linux as the only peer | S–M |
| A7 | **Host unit tests** for the wire code: ARC encode/decode against Inferno-derived vectors, audio packet encode/decode, the flow-request parser, the USB descriptor check. `make -C tests` | catches regressions without a board | S |
| A8 | **USB VID:PID.** Apply for a free PID under pid.codes (VID 0x1209, for open-source hardware) to replace the 0x8A32 placeholder | needed before anyone else plugs it in | S |
| A9 | **Firmware-update path** (decided by Q7, see below) | the box has no buttons or UI | M |

## Phase B — first hardware session (Waveshare ESP32-P4-ETH)

Ordered by "would this force a redesign?":

| # | test | pass | if it fails |
|---|---|---|---|
| B1 | HS enumeration, 32×32, macOS + Linux + Windows; `usb_descriptor_check.py` live | all three list 32 in / 32 out at 48 kHz | Windows-only failure: descriptor work. All fail: DWC2 iso on P4, so drive the endpoints directly |
| B2 | USB loopback build: `usb_loopback_test.py`, 10 min | bit-exact on 32 ch, `usb_sof_missed` ≈ 0, `usb_fb_phase_ppb` bounded | fix the USB engine |
| B3 | 1588 timestamps: `ptp_no_hw_rx_ts`/`ptp_no_hw_tx_ts` stay 0 with audio TX running | lock < 25 s, offset ±1 µs typical | classifier/descriptor quirks on this silicon revision |
| B4 | A5 stress tool: 4 → 8 → 16 flows at fpp 16 | 0 drops at 8 × 4-ch flows (24 k pps) | raise the RX ring (A4), bigger fpp, or limit RX to 8-ch flows |
| B5 | Dante Controller discovery, rename, channel names | 32×32 visible, names persist after reboot | compare captures against Inferno |
| B6 | RX patches from real AoIP devices and Inferno | green, `aoip_rx_late` = 0 | |
| B7 | TX to real receivers (tone build, then USB) | green, clean audio | |
| B8 | full bridge 24 h | no counter growth (see 20-bench-testing M6) | |
| B9 | robustness: cable pull, leader change, host sleep, power cycle, patch while playing | self-recovers; patch glitch gone on USB after A2/A3 (docs/20 M8) | |
| B10 | latency matrix (docs/21): modes C, B, A; default profile 10 → 6 once `lat_usb_out_headroom_min` ≥ 4 with lead 12 | docs/21 pass criteria | lead 16 if headroom < 2 |

## Phase C — own board (single USB-C + RJ45)

Goal: one USB-C carries audio and bus power. The board has RJ45 and no
buttons. Draft blocks:

| block | plan | to verify |
|---|---|---|
| SoC | **ESP32-P4NRW32X** (chip rev v3.2, 400 MHz, 32 MB PSRAM in package, no radio), chip-down + 16 MB flash. Alternative for > 32 ch: **ESP32-S31** (gigabit EMAC + 1588, HS USB, 320 MHz, 512 KB SRAM, has Wi-Fi/BT); preliminary, and `IDF_ENV_BRINGUP` in IDF v6.1 | P4 hardware design guidelines; v3.2 pin 54 change vs v1.3; build with min chip rev v3 + `CPU_FREQ_400` |
| USB-C | UFP receptacle, CC1/CC2 5.1 kΩ to GND, D+/D− to the P4 HS OTG pads (90 Ω diff), low-cap ESD rated for HS, VBUS → load switch / soft start | inrush < USB limit |
| power | VBUS 5 V → buck 3.3 V (+ the P4 core rail per guidelines). Budget P4 + IP101 + magjack LEDs < 2.5 W (500 mA) | measure on the Waveshare board in B8 |
| Ethernet | IP101GRI RMII + 50 MHz oscillator into the P4 REF_CLK (proven on Waveshare) + magjack with LEDs | — |
| debug/flash | Tag-Connect pads: UART0, EN, BOOT(GPIO35), and the P4 USB-Serial-JTAG (FS) D+/D− | |
| status | 1 LED (green = PTP lock + USB streaming; blink codes for faults) | fits "no config": status only, no controls |
| PoE | **not on rev 1**; leave room for a later isolated 802.3af PD + ideal-diode OR | |
| enclosure | small extruded aluminium or printed; RJ45 + USB-C on one end | |

Rev-1 exit: all of Phase B passes on the own board, plus USB-IF-style basic
checks (inrush, enumeration on hubs and long cables).

## Phase D — release

Versioned firmware images, a flashing guide for the chosen update path, a
README for users (what it is, how to patch in Dante Controller, limits:
48 kHz, 32×32, 100 Mbit), a licence notice (GPL-3.0-or-later, Inferno
credit), and the "not affiliated" statement.

## Order

```
now ──► A1 review ─► A2+A3 (flash stalls) ─► A4, A5, A6, A7 ─► A8, A9
                          board arrives ──► B1 → B2 → B3 → B4 ─► B5…B9 ─► Phase C layout
```
A1–A3 come first because B9 depends on them. A5 and A6 make B3–B4 cheap.
Phase C schematic work can start after B1–B4 pass: they settle every
assumption the PCB depends on (HS USB, RMII clocking, power, pins).

## Questions still open

- **Q3 Clock leader:** must the box become PTP leader when no AoIP leader
  exists? Doing so needs a PTPv1 master + BMC and changes A6 into a firmware
  item. *Default: no.*
- **Q5 Hosts:** which matter, in what order? Is Windows ASIO needed? (ASIO
  would need a separate Windows driver; out of scope by default.)
- **Q7 Firmware updates** on the own board (one USB-C):
  - (a) USB DFU interface added to the audio device. Works from any OS with
    `dfu-util`; *recommended*.
  - (b) network OTA (a fixed HTTP endpoint or a push tool).
  - (c) Tag-Connect/UART only.
- **Q9 Own board:** chip-down or module, a rough quantity, and whether PoE
  should be laid out (unpopulated) on rev 1.
- **Q10 Status LED:** is a single status LED acceptable under "no config"?
- Decided by default unless you object: 48 kHz only; Dante Controller patching
  and renames persisted on the box; GPL-3.0-or-later.
