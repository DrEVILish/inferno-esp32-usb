# 20 — Bench testing (Waveshare ESP32-P4-ETH)

## Hardware setup

```
 host PC ──USB-C cable── [board USB-C]          power + UART console (CH343)
 host PC ──USB-C cable── [USB-C receptacle breakout] ──pigtail── [4-pin MX1.25 HS OTG header]
 switch  ──Cat5e──────── [RJ45]
```

- **HS pigtail:** wire the MX1.25 pins to the breakout. D+ goes to D+, D− to
  D−, and GND to GND. Leave 5 V unconnected, because the board is powered from
  its own USB-C. Keep the wire under 10 cm and twist D+ with D−. Check the pin
  order on the Waveshare silkscreen or schematic before soldering.
- **Network:** use a managed switch with QoS (DSCP 56/46 strict priority)
  and IGMP snooping that is either off or has a querier. A real AoIP device
  must be the PTP leader. Run Dante Controller on a PC. Inferno on Linux is
  the second peer.

## Build and flash

```bash
cd /opt/usb-inferno
tools/build.sh                                  # production build
. tools/idf-env.sh && cd firmware
idf.py -p /dev/ttyACM0 flash                    # board USB-C (UART)
```

Test variants: `tools/build-variant.sh USB_LOOPBACK|NET_LOOPBACK|TONE`, or
`idf.py menuconfig` → "usb-inferno build profile", or add one of these lines
to `sdkconfig` before building. Latency profiles and channel counts:
`tools/build-matrix.sh LAT<10|6|4|2>_CH<32|16|8|4|2>[_NETLOOP|_USBLOOP]`
(docs/21-latency-matrix.md). Every build script runs `tools/check_iram.py`.

| build | line | used for |
|---|---|---|
| USB loopback | `CONFIG_USBINF_TEST_USB_LOOPBACK=y` | M1: USB playback ch N comes back on capture ch N, bit-exact |
| network tone | `CONFIG_USBINF_TEST_TONE=y` | M5 without USB: TX ch N = N × 100 Hz sine |
| network loopback | `CONFIG_USBINF_TEST_NET_LOOPBACK=y` | M4+M5 without USB: RX ch N → TX ch N |

Watch the device over the network, not over UART:
`tools/net_stats.py <ip> watch`. Telemetry is served on UDP 7779.

## Test sequence

| step | do | pass |
|---|---|---|
| M1a | USB loopback build. Run `lsusb -v` on Linux and check System Information on macOS | 32 in / 32 out at 48 kHz, async OUT with a feedback EP, max packet 896 |
| M1b | `tools/usb_loopback_test.py` | all 32 channels bit-exact; `usb_out_lead_frames` stable |
| M1c | Windows 10/11: Sound settings and USBView | the device enumerates with 32 ch both ways and plays without error |
| M2 | production build on the network, no USB | `ptp_locked=1` within 25 s; `ptp_offset_ns` mostly within ±1000 |
| M3 | Dante Controller | the device appears with 32 TX / 32 RX and channels 01..32 |
| M4 | subscribe RX 1..32 from the Inferno/AoIP transmitters | patches green; `aoip_rx_late=0`; audio on USB capture |
| M5 | subscribe a receiver to our TX 1..32 | the receiver is green and gets USB playback audio (or the tone build) |
| M6 | full bridge for 24 h | no growth in `usb_*_underruns`, `aoip_rx_late`, `ring_*`; `ptp_epoch` constant |
| M7 | pull the cable, reboot the leader, sleep the host, power-cycle | recovers without help; subscriptions restored |
| M8 | NVS under audio: full bridge streaming both ways, rename 10 channels in DC within 2 s, wait 5 s; repeat 20× | `aoip_nvs_writes` +1 per burst; `usb_out_underruns`, `usb_in_resyncs`, `usb_sof_missed` unchanged (A2: USB keeps running through the flash write); network side may show a short `aoip_tx_lag_resets`/`aoip_rx_late` blip per write |

## Latency

Budget and expected values: docs/14-latency.md. Full matrix (profiles ×
channel counts × modes A/B/C): docs/21-latency-matrix.md. The procedure
below is one permutation; `fw_build` in the telemetry names the image.

Setup: Linux test PC running Inferno (`inferno2pipe` / ALSA plugin) and
JACK on the DUT's USB card; DUT and PC on one switch (profiles 4/2: direct
link or one QoS switch with nothing else on it); one PTP leader (Inferno's
clock or a Dante device). Telemetry: `tools/net_stats.py <ip> watch 1`.

1. **Calibrate (step 0).** Patch Inferno TX 1 → Inferno RX 1 directly. Send
   an impulse (one full-scale sample every 0.5 s) and record; the offset in
   samples is Inferno's own buffering, subtracted below.
2. **Network → USB (one way).** Patch Inferno TX 1 → DUT RX 1. JACK records
   the DUT's capture 1 and, through the same JACK graph, Inferno's RX of a
   PTP-aligned reference; or, simpler, use `jack_iodelay`-style impulse
   matching between Inferno TX (sent at a known media time) and the DUT
   capture. Expected: L + 3 … L + 15 frames + host capture period.
   Telemetry during the run: `lat_usb_in_age_min/max` within
   L + 3 … L + 9 (+ a few for ISR latency); `lat_rx_margin_min` ≥ 0;
   `aoip_rx_late` = 0.
3. **USB → network (one way).** JACK plays the impulse on DUT playback 1;
   patch DUT TX 1 → Inferno RX 1 and record there. Expected: lead + L … lead
   + L + 6 frames + host playback buffering. Telemetry:
   `lat_usb_out_headroom_min` ≥ 2 (lead 12) / ≥ 38 (lead 48),
   `usb_out_lead_frames` = lead ± 1, `lat_tx_send_lag_max` a few frames,
   `aoip_tx_underruns` = 0.
4. **Full loop (mode A).** Inferno TX 1 → DUT RX 1 → JACK connects capture 1
   to playback 1 → DUT TX 1 → Inferno RX 1. Impulse from Inferno, record
   on Inferno RX; RT = measured − step 0. 100 impulses: median ≤ target,
   spread ≤ 1 sample (the device is sample-locked: any spread is a bug).
5. **Device only (mode B).** `LAT<p>_CH<n>_NETLOOP` image: Inferno TX →
   DUT RX → DUT TX → Inferno RX. Expected 2 · L exactly (+ Inferno's own,
   removed by step 0). Spread 0.
6. **Host only (mode C).** `LAT<p>_CH<n>_USBLOOP` image:
   `tools/usb_loopback_test.py --ch <n>` (bit-exact + delay); the delay
   minus `UI_LOOPBACK_DELAY_FRAMES` (192) is the host + USB share.
7. **Record** per run: `fw_build`, telemetry snapshot at start and end,
   packet rates (`aoip_tx_packets`, `aoip_rx_packets` per second), link
   load, `cpu0_idle_pct` / `cpu1_idle_pct`, JACK period/nperiods.
