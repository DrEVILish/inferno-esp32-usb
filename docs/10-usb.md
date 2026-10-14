# 10 — USB (UAC2 32×32, High-Speed)

Status: implemented 2026-10-12. It compiles cleanly and the clock and position
logic passes a host simulation. **It has not been run on hardware.** §9 lists
what is still unverified.

Code: `firmware/main/usb/`

| file | what |
|---|---|
| `uac2_defs.h` | fixed numbers: interfaces, endpoints, entity IDs, string indices, sizes |
| `uac2_desc.c` | device, configuration, qualifier and string descriptors (TinyUSB callbacks) |
| `uac2.c` | TinyUSB glue: our own class driver, ISR data path, SET_INTERFACE, clock requests, PHY and task startup, stats |
| `uac2_engine.c/.h` | clock and position engine: feedback, OUT write position, IN read position. Pure C, testable on a host |
| `tusb/tusb_config.h` | TinyUSB configuration (DWC2 Buffer-DMA, D-cache maintenance, no built-in classes) |

Tools: `tools/usb_engine_sim.sh` (host simulation),
`tools/usb_descriptor_check.py`, `tools/usb_loopback_test.py`.

## 1. Stack choice

- **`espressif/tinyusb` 0.21.0~2 directly, without `esp_tinyusb`.**
  `esp_tinyusb` defines the `tud_descriptor_*_cb` callbacks itself and limits
  the string table to 8 entries. UAC2 channel names need 64 consecutive
  strings. We do the small part we need from it ourselves: the UTMI PHY
  (`usb_new_phy`) and a TinyUSB task pinned to `UI_CORE_USB` at
  `UI_PRIO_USB`. Our `tusb_config.h` is injected into the tinyusb component by
  `main/CMakeLists.txt`.
- **Our own class driver, not TinyUSB's audio class.** TinyUSB's
  `audio_device.c` moves samples through software FIFOs, adding one copy per
  direction. It sizes IN packets from FIFO fill, so it cannot send "exactly
  the media-clock advance per microframe". Its feedback helpers measure
  against the MCU clock, not our PTP media clock. The custom driver (`usbd_app_driver_get_cb`,
  about 400 lines) does everything in the USB interrupt:
  - `.sof`: TinyUSB calls a class driver's SOF hook **in ISR context**
    (`dcd_event_handler`, "SOF driver handler in ISR context"). We latch
    `mc_now_ns()` as the first statement. On DWC2 HS the SOF interrupt fires
    every microframe and `DSTS.SOFFN` is the 14-bit microframe number.
  - `.xfer_isr`: transfer completion in ISR context. We re-arm the endpoint
    and move the data there. Nothing goes through the TinyUSB task queue.
  - Control requests (SET/GET_INTERFACE, clock source) run in the TinyUSB
    task.
- **DMA and cache:** DWC2 Buffer DMA. The P4 reaches internal SRAM through
  the L1 D-cache. TinyUSB cleans IN buffers before DMA and invalidates OUT
  buffers after DMA (`CFG_TUD_MEM_DCACHE_ENABLE`). Every buffer we hand over
  is a static array in internal RAM, aligned to the 64-byte cache line and a
  whole number of lines long (896 = 14 × 64 at 32 ch; at fewer channels the
  buffer is rounded up to whole lines and the packet stays exact). There are two OUT buffers and two IN buffers
  (ping-pong) and one 64-byte feedback buffer. The data path does no malloc and
  no logging.
- **Concurrency:** `tusb_rhport_init()` runs inside the USB task on core 1,
  so the DWC2 interrupt is allocated on core 1. The task's critical sections
  (`s_lock`, a portMUX) therefore also exclude the ISR. This matters for
  `usbd_edpt_iso_activate()`, which polls DIEPINT bits that the ISR would
  otherwise clear under it.

## 2. Descriptor layout

```
Device  bcdUSB 2.00, class EF/02/01 (IAD), EP0 64, VID 0x303A PID 0x8A32 (placeholder, see uac2_defs.h)
Config 1  (218 bytes, 3 interfaces, bus-powered, 500 mA)
  IAD  first 0, count 3, AUDIO / 0x00 / AF_VERSION_02_00, iFunction = product
  itf 0  AudioControl (no interrupt EP)
     AC Header        bcdADC 2.00, category I/O box
     Clock Source 9   internal fixed, bmControls 0x05 (freq r/o, validity r/o)
     IT 1  USB streaming 0x0101, 32 ch, bmChannelConfig 0, iChannelNames 4  ("Net TX 01..32")
     OT 2  digital audio interface 0x0602, source 1
     IT 3  digital audio interface 0x0602, 32 ch, iChannelNames 36  ("Net RX 01..32")
     OT 4  USB streaming 0x0101, source 3
     (no Feature Units: bit-transparent)
  itf 1  AS OUT  alt 0 (no EP) | alt 1: AS_GENERAL link 1, PCM, 32 ch, names 4
                 FORMAT_TYPE_I subslot 4, 24 bit
                 EP 0x01 iso async data   896 B  bInterval 1 (every microframe)
                 EP 0x81 iso feedback       4 B  bInterval 4 (2^3 = 8 microframes = 1 ms)
  itf 2  AS IN   alt 0 | alt 1: AS_GENERAL link 4, PCM, 32 ch, names 36, subslot 4 / 24 bit
                 EP 0x82 iso async data   896 B  bInterval 1
Strings: 1 manufacturer, 2 product, 3 serial (Ethernet MAC), 4..35 "Net TX nn", 36..67 "Net RX nn"
```

At Full Speed the device returns an empty configuration with no interfaces,
because 32 channels do not fit in 12 Mbit/s. The other-speed configuration
descriptors are filled in accordingly.

**Endpoint sizes.** 32 ch × 4 B = 128 B per frame. 48 kHz / 8000 microframes
gives 6 frames per microframe nominal. `wMaxPacketSize` is (6 + 1) × 128 =
**896**, i.e. whole frames. This avoids the `esp_device_uac` bug, where max
packet = nominal + 4 bytes and Windows rejects more than 2 channels. 896 B fits
in one HS isochronous transaction (≤ 1024), so no high-bandwidth bits are set.
The feedback EP gets a 16-word TX FIFO, the DWC2 minimum depth (TinyUSB would
allocate 1 word for 4 bytes).

**Clock requests.**
- GET CUR(SAM_FREQ) returns 48000.
- GET RANGE returns {1, 48000, 48000, 0}.
- GET CUR(CLOCK_VALID) returns 1.
- SET CUR(SAM_FREQ) is accepted only with 48000 and stalled otherwise.
- All other entity requests stall.

## 3. Clock model and feedback maths

The device is the clock master. The media clock is
`media_frame = mc_ns_to_frames(mc_now_ns())`, where `mc_now_ns()` is the EMAC
1588 counter disciplined by PTP.

**SOF timestamps.** Each SOF interrupt latches `t = mc_now_ns()`. ISR latency
is only ever positive, so per block of 64 microframes (8 ms) the engine keeps
the SOF with the **smallest** `t − 125000·uf`. That is the lower envelope:
the timestamp with the least latency. It pushes that SOF into a 128-entry
history, giving a window of about 1.02 s that slides every 8 ms.

**Rate:** frames per microframe from the oldest to the newest history entry,
in 8.24 fixed point:

```
fb_raw_q24 = Δns · (fs/8000 · 2^24) / (125000 · Δuframes)      (= 6·2^24 at nominal)
```

The units are media ns over USB microframes, i.e. media frames per host
microframe. That is exactly what the host must send. The measurement is
trusted after 16 intervals (128 ms); before that, and across a restart, it
keeps the previous value (nominal at boot).

**Phase term:** keeps the OUT lead centred.

```
lead      = w − F(arrival microframe SOF)      (frames; IIR, 128 packets ≈ 16 ms)
err       = lead − UI_USB_OUT_LEAD_FRAMES (48 at profile 10, 12 at 6/4/2)
phase_ppb = clamp(−err · 1000 ppb/frame, ±10 000 ppb)        (±10 ppm max)
fb_q24    = fb_raw_q24 · (1 + phase_ppb·1e−9)
```

The loop is first order, with time constant 1 / (1 ppm/frame × 48000 frames/s)
≈ 21 s. Its only job is to remove the offset left by anchoring and the
residual estimation error. The phase term is 0 while the host is not
streaming OUT.

**Wire format:** HS 16.16 frames per microframe, 4 bytes little-endian, sent
every 1 ms. Each feedback packet carries `fb_q24 >> 8`. The 8 bits dropped
are carried by first-order error feedback (dithering) into the next packet,
so the host's 16.16 accumulator sees sub-LSB precision. One LSB is 2.5 ppm.

**Fitted SOF line.** Once 2 history intervals exist, the engine computes
`sof_f` from the *fitted* SOF time instead of the raw timestamp: last
envelope point plus `uf` × fitted period. The IN frame count then follows a
clean line (only 5/6/7 frames) whatever the ISR jitter. The fitted time sits
on the lower envelope, so it is never later than the real read. Single late
SOFs (more than 50 µs) are counted (`sof_late`). If 8 consecutive SOFs fall
more than 2 ms off the line, or are early, the fit restarts (`sof_refits`).
Missed SOF interrupts are counted from the DWC2 frame number. The engine
cross-checks that number against the timestamps (`sof_fn_bad`) and falls back
to the timestamps if they disagree. If the core turns out to report 1 ms frame
numbers instead of microframes, it still works; this case is simulated.

## 4. OUT path (host playback → network TX)

- **Write position `w`** in `g_ring_usb_to_net`.
- **Idle** (alt 0, no packet for 32 microframes, or no packet yet): every SOF
  writes silence up to `sof_f + lead`, so network TX carries silence and `w`
  stays at now + lead. When the host starts streaming it continues from there
  without a re-anchor.
- **Streaming:** each completed OUT transfer is handled in the ISR. The ISR
  re-arms the other buffer first, then calls `ring_write_frames(w, n)`, then
  `w += n`.
- **Arrival microframe.** A transfer armed in microframe A receives the
  packet of A+1, however late the interrupt runs. The lead is measured at that
  arrival SOF, so it does not depend on ISR latency. The exception: if the ISR
  is demonstrably on time but 2 or more microframes passed since arming,
  earlier packets were lost and this one arrived now.
- **Lost packets.** A gap in arrival microframes means lost packets. The
  engine writes silence for (gap−1) × feedback frames, cross-checked against
  the measured lead drop, so the host's stream stays aligned to the media
  clock (`out_gap_fills`, `out_gap_frames`). In simulation, single losses and
  1 ms ISR blackouts keep the loopback delay exactly constant.
- **Gross errors** (counted, then `w = now + lead` with silence backfill):
  - lead < max(4, lead/2), or the frames about to be written less than
    (that − 6)/2 ahead of the real now (lead 48: 24 / 9; lead 12: 6 / 0):
    `out_underruns`
  - lead > 480: `out_overruns`
- **Epoch change** (`mc_epoch()` differs at a SOF): both positions are
  re-anchored, the rate history restarts (the rate value is kept), and OUT
  restarts.

## 5. IN path (network RX → host capture)

- **Read position `r`.** Each IN packet carries frames `[r, target)` with
  `target = sof_f − UI_NET_LATENCY_FRAMES − UI_USB_IN_MARGIN_FRAMES (3)`
  (was one microframe, 6; docs/13 §2 for why 3 is enough).
  Consecutive packets therefore carry exactly the media-clock advance between
  SOFs: 6, sometimes 5 or 7. `r` is continuous.
- **Behind by more than 7** (after an ISR stall): packets of 7 frames until
  caught up (`in_catchup`). No samples are lost and the delay stays constant.
- **Re-anchor** only when `target − r` is below 0 or above 64, or on an epoch
  change (`in_resyncs`, `in_underruns`). On re-anchor, the past part of the
  ring (all but the newest 128 frames) is zeroed, so no stale lap is ever
  played. This is a 48 KB memset in the ISR at 32 ch: rare (boot, epoch, replug).
- **Not streaming:** every SOF clears `[r, target)` and advances `r`. Network
  RX is discarded and `r` stays continuous.
- **Bound for AoIP RX late detection:** frame f is read at media time
  ≥ f + L + 3, never earlier.
- **Data source per build:**

  | build | source |
  |---|---|
  | production | `ring_read_frames_and_clear(g_ring_net_to_usb)` |
  | `CONFIG_USBINF_TEST_USB_LOOPBACK` | non-clearing read of `g_ring_usb_to_net` at now − `UI_LOOPBACK_DELAY_FRAMES` (192); unwritten frames read as zero |
  | `CONFIG_USBINF_TEST_NET_LOOPBACK` | silence; `g_ring_net_to_usb` is never read or cleared (AoIP TX reads it) |

## 6. Telemetry (`uac2_get_stats`)

The original fields keep their meaning: `out_lead_frames` is the smoothed
lead while streaming, otherwise `w − now`. These fields were added:

| field | meaning |
|---|---|
| `sof_missed` | SOF interrupts never seen |
| `sof_late` | SOF timestamps > 50 µs after the fitted line |
| `sof_gaps` | SOF stream restarts (suspend, unplug, gap > 100 ms) |
| `sof_fn_bad` | DWC2 frame number disagreed with timestamps |
| `epoch_changes` | media-clock steps handled |
| `out_packets` / `in_packets` | packets transferred |
| `out_gap_fills` / `out_gap_frames` | lost OUT packets replaced by silence |
| `out_bad_len` | OUT packets that were not whole frames |
| `in_catchup` | IN packets capped at 7 frames while behind |
| `in_resyncs` | IN re-anchors |
| `xfer_errors` | failed isochronous transfers |
| `arm_failures` | endpoint arming refused, e.g. bus-reset races |
| `fb_phase_ppb` | current phase term |
| `configured`, `high_speed`, `out_alt`, `in_alt` | state |

Latency windows (reset on each read, docs/14-latency.md §7):
`out_hr_min/max` (OUT data first frame − real media clock at the write: the
lead actually achieved) and `in_age_min/max` (real media clock − newest
frame of an IN packet when built). Telemetry prints them as
`lat_usb_out_headroom_*` and `lat_usb_in_age_*`.

## 7. How to test

**Host simulation (no hardware):** `tools/usb_engine_sim.sh` builds
`uac2_engine.c` with plain gcc in the production and the loopback variant,
for every latency profile (10/6/4/2) and once at 2 channels. It runs 9
scenarios (the two "late OUT" ones and the TX headroom metric were added
for the latency work, docs/14-latency.md §3):
- nominal clocks
- ±30..100 ppm host and media offsets
- a lost OUT packet every 7 s
- a +10 ms media-clock step
- a 1 ms ISR blackout every 3 s
- DSTS reporting frame numbers
- OUT completion anywhere in the microframe, with ISR latency spikes, alone
  and with a lost packet every 3 s

It models DWC2 iso OUT arming with parity, host packet sizing from the 16.16
feedback, and ISR latency jitter. Checks: feedback rate within 3 ppm (seen:
< 0.5 ppm), lead held within 3 frames of the target, IN sizes only 5/6/7, no OUT
re-anchors, and in loopback, every captured frame bit-exact at a constant
delay.

**Descriptors:**
- From the build: `tools/usb_descriptor_check.py --elf firmware/build/usb_inferno.elf`
  (run with the ESP-IDF python, which has pyelftools). It passes today.
- On a Linux host with the device attached: `tools/usb_descriptor_check.py --sysfs`
  checks the live descriptors.
- `lsusb -v -d 303a:8a32` shows the full tree and resolves the channel-name
  strings. Check:
  - `bcdADC 2.00`
  - Clock Source `bmAttributes 0x01`
  - endpoints `Transfer Type Isochronous`, `Synch Type Asynchronous`,
    `wMaxPacketSize 0x0380 1x 896 bytes`, `bInterval 1`
  - feedback EP `Usage Type Feedback`, `bInterval 4`
  - `iChannelNames 4 Net TX 01`, and `36 Net RX 01` on the IN side
- Windows: USBView (Windows SDK) shows the same tree. Device Manager should
  bind "USB Audio 2.0" (`usbaudio2.sys`), and the playback and recording
  devices should show 32 channels.
- macOS: Audio MIDI Setup should show 32 in / 32 out at 48 kHz with the
  channel names. `system_profiler SPUSBDataType` shows the link speed (must be
  480 Mb/s).

**M1 bit-exact round trip:**
1. Build with `CONFIG_USBINF_TEST_USB_LOOPBACK=y`.
2. Flash, and connect the HS port (MX1.25 pigtail) to a Linux host.
3. Run `tools/usb_loopback_test.py`. It finds the card in
   `/proc/asound/cards`, or use `-D hw:N,0`; `-t 60` runs for longer.
4. It plays a 32-channel, per-channel-unique 24-bit pattern (S32_LE,
   left-justified, never zero) with `aplay`, records 32 channels with
   `arecord`, and finds the delay. It reports any discontinuity (lost or
   repeated blocks) and the per-channel result. PASS means all 32 channels
   are bit-exact over the whole overlap with a single constant offset.
5. While it runs, check the UDP telemetry: `usb_out_overruns`,
   `usb_out_underruns` and `usb_in_underruns` must stay 0, and
   `usb_feedback_q16` must be about 393216 ± ppm.

`--backend sounddevice` uses python-sounddevice and numpy instead of
alsa-utils.

## 8. Resources

- **Interrupts:** about 8 k SOF + 8 k OUT + 8 k IN + 1 k feedback per second
  on core 1.
- **Copies per packet:** one memcpy into the ring (OUT, 768 B), one memcpy
  plus memset out of the ring (IN). Each ISR is a few µs (estimate).
- **RAM:**
  - DMA buffers 3.6 KB, engine state about 2.5 KB, all internal.
  - Engine and ISR functions are in IRAM, and so is TinyUSB's interrupt
    path (A2, below); `tools/check_iram.py` verifies it after every build.
  - The TinyUSB task stack is 4 KB.

## 9. Unverified without hardware / risks

1. **Never enumerated on a real host.** The descriptors pass our checker, but
   macOS, Windows `usbaudio2.sys` (32 ch, async with feedback) and Linux are
   untested. The placeholder PID must be replaced before release.
2. **DWC2 iso behaviour on the P4:**
   - Assumed: `SOFFN` is the HS microframe number (with a fallback if it is
     not).
   - Assumed: an iso OUT transfer armed in microframe A receives A+1's
     packet. Re-arming happens in the completion ISR.
   - The incomplete-iso-IN retry in TinyUSB gives 1 retry at bInterval 1 and
     8 retries for the feedback EP at bInterval 4.
   - TinyUSB does not handle incomplete iso OUT (`incompISOOUT`). If the OUT
     endpoint ever gets stuck on the wrong parity, `out_streaming` drops and
     the idle path writes silence until the next SET_INTERFACE.
3. **Flash operations and the USB ISR (fixed, A2, 2026-10-13).** TinyUSB
   allocates the DWC2 interrupt without `ESP_INTR_FLAG_IRAM`. `uac2.c` wraps
   `esp_intr_alloc` at link time (`-Wl,--wrap=esp_intr_alloc`, set in
   `main/CMakeLists.txt`) and adds the flag for `ETS_USB_OTG_INTR_SOURCE`;
   `main/linker.lf` places `dcd_dwc2`, `usbd` and `tusb_fifo` in IRAM (their
   rodata, e.g. `_dwc2_controller`, in DRAM). `tools/check_iram.py` walks the
   call graph from the ISR roots after each build and fails the build on any
   flash-resident function or flash/PSRAM constant on the path; `uac2_start`
   logs an error if the wrap did not apply. Still to confirm on hardware
   (B9): an NVS write during streaming leaves `usb_*_underruns`,
   `usb_in_resyncs` and `usb_sof_missed` unchanged.
4. **ISR load:** about 25 k interrupts/s on core 1. The estimate is a few µs
   each; measure it.
5. **Time and epoch are read as a pair** in the SOF ISR
   (`mc_now_ns_epoch()`, a seqlock against `mc_hw_step`, IRAM). The real
   `mc_now_ns()` is IRAM-safe (net/clock module).
6. **The `CONFIG_USBINF_TEST_USB_LOOPBACK` firmware build has not been
   compiled with the IDF toolchain.** The engine compiles cleanly with host
   gcc in all three variants, and the glue does not depend on the option.
7. **Bus power:** the board draws its power from the UART USB-C. The HS
   header's VBUS is not sensed, so unplug is only seen as a SOF gap or
   suspend.
