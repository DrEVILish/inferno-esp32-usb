# 01 — Architecture

Status: draft 1, 2026-10-12. Numbers marked **(verify)** are estimates still to
be measured on the bench.

## 1. What the box is

```
            RJ45                                          USB-C (HS 480 Mbit/s, bus power)
   ┌────────────┐   RMII   ┌───────────────────────────┐   UTMI  ┌──────────┐
   │ magjack +  │──────────│        ESP32-P4 (≥v3.1)   │─────────│ host PC  │
   │ IP101 PHY  │          │  EMAC + IEEE1588 clock    │         │ (UAC2)   │
   └────────────┘          │  2 × RISC-V HP            │         └──────────┘
                           │  768 KB L2MEM, PSRAM      │
                           └───────────────────────────┘
```

- Network: 32 TX + 32 RX channels, 48 kHz, 24-bit, PTPv1 follower.
- USB: UAC2, one 32-ch OUT (playback) + one 32-ch IN (capture), 48 kHz,
  24-bit in 4-byte subslots, **asynchronous** with an explicit feedback
  endpoint.
- Mapping (fixed): `USB OUT ch N → network TX ch N`, `network RX ch N → USB IN ch N`.
- There are no I2S, codec chips or ASRC. The "codec" is the USB audio function itself.

## 2. Why ESP32-P4 (and nothing smaller)

| need | S3 | P4 |
|---|---|---|
| 32 ch × 48 kHz × 4 B = 6.1 MB/s each way over USB | FS 12 Mbit/s: ~2–4 ch max | **HS 480 Mbit/s**, UTMI PHY on chip |
| Ethernet MAC | none (SPI W5500 only) | **EMAC, RMII, 10/100** |
| PTP hardware timestamps | no | **IEEE 1588 system time + TX/RX stamps** |
| proven AoIP receive | — | Nickster90s port: PTPv1 lock, 96 kHz 8-ch RX |

**Limit to note:** the P4 EMAC is **Fast Ethernet only** (MII/RMII, no RGMII).
That sets the sample rate (see §4).

## 3. Clock architecture (the core design decision)

There is a single time base: the **EMAC IEEE 1588 system time**, slaved to the
network's PTPv1 leader by a servo. The EMAC addend register is trimmed, so the
hardware clock *is* PTP time. The media clock is derived from it
arithmetically:

```
media_sample(t) = floor(ptp_time_ns(t) × 48000 / 1e9)       (64-bit, never wraps)
```

Both sides hang off that number:

- **Network TX:** packet k carries timestamp `media_sample` = k × fpp. A packet
  is built when the media clock passes that index + lead.
- **Network RX:** each received packet's timestamp says where it sits in the RX
  ring. Playout position = `media_sample − rx_latency`.
- **USB, asynchronous mode:** the device is the USB clock master, so the
  *host* follows *us*. At each SOF (microframe start) the ISR latches the 1588
  time. Then:
  - **IN (capture):** the microframe carries `media_sample(SOF_k) − media_sample(SOF_k−1)`
    frames (normally 6, sometimes 5 or 7), read from the RX ring.
  - **OUT (playback):** the feedback endpoint reports frames-per-microframe in
    16.16. It is measured as the media-clock frame count over a window of
    SOFs, plus a small bounded phase term (±10 ppm max) that keeps the OUT ring
    fill centred. The host then sends exactly the network's rate.

The result is no sample-rate conversion anywhere and bit-transparent audio. One
servo, the PTP servo, defines the rate. The USB feedback only *reports* that
rate, and the fill-centring term is the only other control loop.

- **No leader on the network:** the 1588 clock free-runs on the crystal and USB
  works unchanged. Network audio needs a leader. Whether the box should
  *become* the leader is open question Q3.
- **APLL:** not used for audio. The RMII 50 MHz reference comes from the PHY or
  an oscillator, as on the Waveshare boards.

## 4. Bandwidth budgets

### Ethernet (100 Mbit/s full duplex)

Wire overhead per packet is about 66 B: preamble, IFG, Ethernet, IP and UDP
headers, FCS, plus the AoIP header. Inferno's TX limit is
`MAX_CHANNELS_IN_FLOW = 8`, so 32 TX channels need **at least 4 flows**.

| case (48 kHz, 24-bit) | packet | pps | Mbit/s |
|---|---|---|---|
| 4 × 8-ch flows, fpp 16 | 384 + 66 B | 12 000 | ~43 |
| 4 × 8-ch flows, fpp 32 | 768 + 66 B | 6 000 | ~40 |
| RX worst plausible: 8 × 4-ch flows, fpp 16 | 192 + 66 B | 24 000 | ~50 |
| same at 96 kHz | | 48 000 | ~100 — **does not fit** |

→ **48 kHz fixed** for 32×32. A 96 kHz 32×32 setup cannot fit the P4's 10/100
MAC. Multicast TX flows count once each regardless of receiver count; unicast
flows count per receiver. A per-receiver unicast fan-out is capped by the link,
so cap TX at e.g. 2 unicast receivers × 32 ch, or 1 × 32 + multicast
**(verify)**.

### USB High-Speed

32 ch × 4 B × 6 frames = 768 B per microframe nominal, 896 B max (7 frames).
This fits a single 1024 B isochronous transaction (bInterval 1 = 125 µs). At
8000 packets/s in each direction it uses about 57 Mbit/s each way, under 15 %
of HS.

### CPU (P4, 2 cores)

| work | estimate |
|---|---|
| sample format conversion (24-bit BE ↔ 32-bit LE), 2 × 1.54 M samples/s | ~3–5 % of one core |
| per-packet RX/TX handling, 12–24 k pps each way, ~2–4 µs each | 10–20 % of one core **(verify)** |
| USB ISR + DMA, 16 k transfers/s | ~5–10 % of one core **(verify)** |
| PTP, control plane, mDNS, lwIP | small |

The packet rate is the real budget risk, not the sample count. It is mitigated
by decoding audio inside the EMAC RX path, which bypasses lwIP. The P4 port
already proves this approach. TX audio is sent as prebuilt raw frames, not
lwIP UDP.

### Memory (internal L2MEM, DMA-capable, never PSRAM for audio)

32 ch × 4 B × 48 k = 6.1 KB/ms. Each direction gets a 512-frame ring
(10.7 ms, 64 KB at 32 ch; it scales with `UI_CHANNELS`), so both together take
128 KB (was 2 × 1024 frames = 256 KB, which did not fit, see §8). Ethernet DMA takes 64 RX × 1600 B ≈ 102 KB plus 30 TX × 1600 B ≈ 48 KB.
IDF v6.1 caps the RX ring at 30 descriptors; a project-local `esp_eth`
override raises it to 64 (A4, `patches/esp_eth-rx64.patch`), because the
P4 port found 20 too few at ~3k pps and we expect up to 24k pps (48k at the
low-latency profiles). That still leaves room (table in §8).

## 5. Core and task layout

| core 0 — real-time network | core 1 — USB + control |
|---|---|
| EMAC RX task (highest prio): demux by UDP port → PTP event / audio decode → ring; everything else → lwIP | TinyUSB device task: OUT → TX ring, RX ring → IN, feedback EP |
| PTP v1 follower + servo (HW stamps, addend trim) | lwIP tcpip, mDNS, ARC 4440, CMC 8800, info 8700, flow control 4455 |
| TX packetizer (prio 22): esp_timer one-shot at the next packet boundary → build flows from TX ring → raw `esp_eth_transmit` | heartbeat, telemetry (UDP), NVS persistence |
| PTP follower at prio 20, *below* the packetizer (its Delay_Req busy-waits) | USB ISR (SOF, OUT, IN, feedback): allocated IRAM-safe, whole path in IRAM (A2, `tools/check_iram.py`) |

Rules carried over from the P4 port (README "Lessons", "Known issues"):
`CONFIG_ETH_TRANSMIT_MUTEX=y`, 64 RX DMA buffers (local esp_eth override), `-O2`, no UART logging in
hot paths (use UDP telemetry), 64-bit sample indices everywhere, and IGMP on.

## 6. AoIP behaviour (fixed)

- Name `INFUSB-xxxxxx` (last 3 MAC bytes). Channels `01`..`32` both ways.
- 48 kHz, 24-bit. Device latency 1 ms by default; build-time latency
  profiles 10/6/4/2 select 1 / 1 / 0.5 / 0.25 ms (docs/14-latency.md).
- Channel count is a build-time setting (`UI_CHANNELS` 2/4/8/16/32, default
  32) for the test matrix (docs/21).
- TX: Inferno-compatible flow control server (unicast requests from receivers)
  plus multicast flows. Up to 8 ch per flow.
- RX: subscriptions made from the controller are persisted in NVS and restored
  at boot, with no user action (Q2).
- Link-local or DHCP addressing, automatic. No web UI, buttons or display.
- Boot → audio target: < 25 s, set by PTP lock. The P4 port reached green sync
  about 20 s after power-on.

## 7. USB behaviour (fixed)

- UAC2, class-compliant, with no driver on macOS or Linux. On Windows 10/11 it
  uses the in-box `usbaudio2.sys` (32-ch support **(verify)**; ASIO would need
  a third-party driver).
- One clock source entity: internal, fixed 48 kHz, `bmControls` read-only.
- Interface alt 1 per direction: 32 ch, 4-byte subslot, 24-bit resolution.
  The feedback EP is 4 bytes (HS 16.16).
- Channel name strings `Net TX 01..32` / `Net RX 01..32` (iChannelNames).
- When the host isn't streaming, network TX sends silence and RX is discarded.
- No volume or mute controls: the device is bit-transparent.
- Stack: TinyUSB (via `esp_tinyusb`) with our own descriptors and feedback
  logic. **Not** `esp_device_uac`, which uses FIFO-fill feedback and has a
  hard-coded `+4` max-packet bug for >2 channels (esp32.com report).

## 8. Hardware

PoE is out of scope for now (decision 2026-10-12). The board is powered from
the USB-C port, so it is bus-powered by the host.

**Dev board: Waveshare ESP32-P4-ETH** (decision 2026-10-12). It is the
same board the Nickster90s P4 port runs on: ESP32-P4 (360 MHz), IP101 PHY on
RMII with an external 50 MHz REF_CLK into GPIO50, 32 MB NOR flash, 32 MB in-package PSRAM, 768 KB HP L2MEM, 8 KB TCM,
RJ45 10/100. A PoE variant exists, but PoE is not used.

The chip revision decides the clock. Rev v1.x runs at 360 MHz, which is the
current build. Rev v3.x runs at 400 MHz: set the minimum chip revision to v3
and `CPU_FREQ_400`. `esptool.py chip_id` prints the revision at the first
flash.

Internal memory budget (768 KB HP L2MEM; checked against the linked image
2026-10-13, production build):

| use | size |
|---|---|
| L2 cache (`CONFIG_CACHE_L2_CACHE_128KB`, the minimum), 0x4FF00000–0x4FF20000 | 128 KB |
| reserved for ROM data/stack at the top (0x4FFADFC0–0x4FFC0000) | 72 KB |
| static: IRAM code 86 KB (incl. TinyUSB ISR path, A2), data 14 KB, bss 89 KB | 188 KB |
| **heap** (`_heap_start` 0x4FF4E0A0 … 0x4FFADFC0) | **≈ 393 KB** |
| — audio rings, 2 × 512 frames × 32 ch × 4 B | 128 KB |
| — EMAC DMA, 64 RX + 30 TX × 1600 B (+ descriptors) | ~152 KB |
| — left for task stacks (~60 KB), lwIP/mDNS, in-flight RX frame copies (1.6 KB each) | ≈ 113 KB |

PSRAM takes allocations > 4 KB that are not DMA/internal-capped
(`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`). **Finding (docs/13 #16):** the
merged design (2 × 1024-frame rings = 256 KB + 30 + 30 EMAC buffers =
96 KB) left ≈ 40 KB for everything else and would most likely have failed
at boot; the rings are now 512 frames. Telemetry `heap_internal_free`,
`heap_internal_min_free` and the boot log line show the real margin; if it
is short on hardware, the next lever is 48 RX descriptors.

The 8 KB TCM (zero-wait) is a candidate for the SOF/feedback ISR hot path if
latency measurements ask for it.

- **USB-C on the board** is power plus UART console (CH343). Opening the
  serial port can reset the chip through DTR/RTS.
- **The audio USB** is the P4's HS OTG, on the **4-pin MX1.25 header**
  (5V, D−, D+, GND). For the bench, make a short pigtail to a USB-C receptacle
  breakout, or a USB-C plug cable to the host. Keep it short and twisted,
  because this is 480 Mbit/s.
- **Power:** the board is powered through its USB-C (UART) port. The HS
  header's 5 V is not wired back to the board supply. For a single-cable setup
  later, bus power arrives from the HS port on our own PCB.
- **Firmware/console:** flash over the board USB-C (UART). Telemetry runs over
  UDP 7779.
- **Pins in use:** Ethernet 28–31, 34, 35, 49–52, and the HS USB PHY pads.
  The onboard ES8311 codec (GPIO9–13) is unused.

Alternative: the Espressif ESP32-P4X-Function-EV-Board puts HS OTG on a real
USB-C (see git history of this file).

**Later — own PCB:** a single USB-C (HS OTG, bus-powered, CC 5.1 k
pull-downs, HS-rated ESD), IP101GRI + magjack, ESP32-P4 rev ≥ v3.1, 16 MB
flash, and a debug UART header. PoE can be added back later as an isolated
802.3af PD with ideal-diode OR-ing onto 5 V.

## 9. Software stack

- ESP-IDF, latest stable (≥ 5.5), written in C (like the P4 port; the IDF EMAC,
  1588 and TinyUSB are all C).
- Protocol knowledge comes from Inferno (GPL-3.0+/AGPL-3.0+) and bench
  captures. The licensing posture is open question Q6.

```
main/
  app.c            boot sequence, task creation, watchdog
  clock/           ptpv1.c, ptp_servo.c, mediaclock.c (1588 time ⇄ samples)
  net/             emac_hook.c (RX demux, raw TX), aoip_rx.c, aoip_tx.c,
                   flowctl.c (4455 server + client), arc.c, cmc.c, info.c, mdns.c
  usb/             uac2_desc.c, uac2.c (EP handlers, SOF latch, feedback)
  audio/           ring.c (64-bit indexed rings), convert.c (BE24 ⇄ LE32)
  sys/             nvs_store.c (subscriptions), telem.c (UDP key=value)
```
