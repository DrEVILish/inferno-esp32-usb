# 12 — AoIP module (`firmware/main/aoip/`)

The AoIP side of usb-inferno: a fixed 32 TX × 32 RX Inferno-compatible
(Dante-protocol) device at 48 kHz / 24 bit, latency `UI_NET_LATENCY_NS`
(1 ms). Inferno (`third_party/inferno-upstream/inferno_aoip/src`) is the
protocol specification; every wire constant is copied from it
(`aoip/proto.h` names the source file of each). The Nickster90s ESP32-P4
receiver was read for facts and pitfalls only.

## 1. Identity

| item | value |
|---|---|
| friendly name (hostname) | `UI3232-xxxxxx` (last 3 MAC bytes, lowercase hex); controller may rename (persisted) |
| factory hostname | `UI3232-<device id hex>` |
| factory device id | EUI-64 of the MAC: `m0 m1 m2 ff fe m3 m4 m5` (same in CMC, mDNS `id=`, multicast headers) |
| process id | 0 |
| board name | `UI3232` (router_info, board/product info) |
| manufacturer / model | `UI_MANUFACTURER` / `UI_MODEL_NAME`; mDNS `model=` stays Inferno's `_000000000000000b` |
| vendor (multicast header) | `"Audinate"` (Inferno's `vendor_string` truncated to 8) |
| channels | TX and RX `01`..`32`; friendly names default to the factory names, renamable |
| format | 48000 Hz, 24 bit, pcm type 0x0e; flow requests for 16/24/32 bit accepted |

## 2. Ports and tasks

| port | what | where |
|---|---|---|
| 4440 | ARC | ctl task |
| 8800 | CMC | ctl task |
| 8700 | info requests; source of 224.0.0.231:8702 and 224.0.0.233:8708 | ctl task |
| 4455 | flow control server (receivers request our TX flows) | ctl task |
| 14336+i | RX flow slot i (unicast audio in, keepalives out) | EMAC hook / ctl task |
| 4321 | multicast media (RX bundles we join; dst of our TX bundles) | EMAC hook |
| 14400+i | source port of TX flow slot i; receivers' keepalives arrive here | EMAC hook |
| 5353 | mDNS (espressif/mdns task, core 1) | |

14336/14400 are our choice (Inferno uses ephemeral ports); fixed ranges let
the EMAC fast path claim the frames.

| task | core | prio | role |
|---|---|---|---|
| `aoip_ctl` | 1 | `UI_PRIO_CONTROL` | select() over ARC/CMC/info/4455; heartbeat 1 Hz; TX expiry + ARP (100 ms); RX keepalives (250 ms); NVS flush; IP watch (200 ms) |
| `aoip_sub` | 1 | `UI_PRIO_CONTROL-1` | subscriber: mDNS lookups, flow requests (block for seconds) |
| `aoip_tx` | 0 | `UI_PRIO_TX_PACKETIZER` | packetizer, woken by a one-shot esp_timer (ISR dispatch → notify) at the next packet boundary |
| EMAC RX (net/) | 0 | 23 | our hooks: RX decode, TX keepalive stamps |

Shared control state is under one recursive mutex (`aoip_lock`). The audio
paths take no locks: the TX table is published to the packetizer as a whole
copy (double buffer, generation + ack), RX slots are reconfigured only while
inactive (+2 ms quiet time) and their channel maps are double-buffered.

## 3. Message flows

**Discovery.** mDNS publishes `<host>._netaudio-arc._udp` (4440),
`<host>._netaudio-cmc._udp` (8800), one `<ch>@<host>._netaudio-chan._udp`
(4455) per TX channel factory name (TXT `default`) plus one per friendly name
that differs, and `<id>@<host>._netaudio-bund._udp` per active multicast TX
flow. TXT records and their order are Inferno's (`mdns_server.rs`). The
controller's first unicast is CMC 0x1001; then ARC. Board and product info
multicasts go out when an address appears and on request (0x61/0xc1);
network info (0x13), clock stats (0x21, only once a PTP leader is known),
0x77→0x78 on request. Heartbeat every second: 0x8001 frequency offset
(`mc_freq_ppb()`), 0x8002 peaks (32 TX + 32 RX, `-40·log10`), 0x8003
per-RX-slot latency (max of now − packet timestamp, swapped to 0 each beat).

**TX (we transmit).** Receiver → 4455 request flow (0x0100). Checks: channel
ids ≤ 32, rate 48000 (else 0x0301), fpp ≤ 256, bits 16/24/32, ≤ 8 channels,
packet ≤ 1472 B (else 0x0302), table full → 0x0315. Reply: 6-byte handle
(u32 slot, u16 random cookie). Same destination again = channel update.
0x0101 stop, 0x0102 update (0x0103 unknown handle). Receivers keepalive our
source port; 4 s without → flow expired (stops sending, reaped at the next
add, revived by an update). Packet: `02`, u32 seconds, u32 sub-second
samples, interleaved BE samples; the timestamp is the TX ring frame index of
its first sample; sent once `mc_now_frames() ≥ next_ts + fpp`, DSCP 46, via
`net_udp_send_raw`. A flow re-anchors (`next_ts` = next multiple of fpp) on a
clock epoch change, a lag > `UI_NET_LATENCY_FRAMES`, or a backward jump.
Multicast TX: ARC 0x2201 creates a bundle on 239.255.x.y:4321 (fpp
`UI_TX_FPP_MAX`: 32 at 1 ms latency, 8 at 0.5 ms, 4 at 0.25 ms),
active after a 1.2 s grace period; 0x2202 deletes.

**RX (we receive).** ARC 0x3010 → subscriber: dealias, reuse sources
already received, mDNS (SRV + TXT `nchan id enc dbcp1 fpp latency_ns b.N`),
group per transmitter, reuse free slots in existing flows (0x0102 update),
join bundles (`net_mcast_join`), else request new flows of min(nchan, 8)
channels with fpp = min(fpp_max, 1400 / (n·bytes), `UI_TX_FPP_MAX`) to our
slot port (the last term keeps the transmitter's packetisation inside our
latency profile, docs/14-latency.md).
Keepalive `13 37` every 250 ms from the slot port to the audio source
address. Status per channel: unresolved (1) → in progress (8) → receiving
(0x01010009 / 0x0101000a), tx fail (0x15); every change sends the
channel-change multicast (07 2a 01 02) so the controller re-reads and shows
green. A flow with no packets for 2 s (if older than 6 s) is torn down and
its channels re-resolved (follows a transmitter that rebooted on a new IP).

**RX decode (EMAC hook).** `frame = sec·48000 + subsec`; written to
`g_ring_net_to_usb` at `frame + shift`, `shift = flow latency − 1 ms`
(flow latency = max(transmitter `latency_ns`, ours)); samples → left-justified
int32 per a precomputed (offset → slot) map. Counters: late (first frame
already due at `now − UI_NET_LATENCY_FRAMES`), early (beyond the ring
window), wrong length (not a whole number of frames), timestamp gaps.

## 4. Translation map

| our file | Inferno source |
|---|---|
| `wire.c` | `protocol/req_resp.rs`, `proto_arc.rs` (serialize_items, paginate, deserialize_items), `util/bytes.rs` |
| `arc.c` | `device_server/arc_server.rs`, `protocol/proto_arc.rs` |
| `cmc.c` | `device_server/cmc_server.rs`, `protocol/proto_cmc.rs` |
| `info_mcast.c` | `device_server/info_mcast_server.rs`, `protocol/mcast.rs`, `peaks.rs` |
| `flowctl_server.c` | `device_server/flows_control_server.rs` |
| `flowctl_client.c` | `protocol/flows_control.rs` |
| `flows_tx.c` | `device_server/flows_tx.rs`, `samples_utils.rs` (16-bit TPDF dither) |
| `tx_multicasts.c` | `device_server/tx_multicasts.rs` |
| `subscriber.c` | `device_server/channels_subscriber.rs` |
| `flows_rx.c` | `device_server/flows_rx.rs` |
| `aoip_mdns.c` | `device_server/mdns_server.rs`, `mdns_client.rs` |
| `settings_nvs.c` | `saved_settings.rs`, `state_storage.rs` |
| `aoip.c` | `device_info.rs`, `settings.rs`, `device_server/mod.rs` |

Verified offline: the ARC 0x3000 reply of a host build is byte-identical to a
capture from an Inferno-mirroring unit (`/var/tmp/inferno-review/arc.pcap`,
32 RX, including Inferno's "24th item written but not counted" paging
quirk); 0x1000 matches except max TX flows (16 here), and the heartbeat
layout matches that unit's 288-byte heartbeats.

## 5. Persistence (NVS namespace `aoip`, version key `ver` = 1)

| key | content |
|---|---|
| `devname` | friendly device name (absent = default) |
| `txnames`, `rxnames` | 32 × 32 B friendly channel names ("" = factory) |
| `subs` | 32 × (tx channel name[32], tx device name[32]) |
| `txmc` | 16 × {flow id, n, channel ids[8], dst ip} multicast TX flows |

Write policy (A3, 2026-10-13): a change only marks its record dirty; the
ctl task writes once 2 s have passed without a further change, and only the
records whose serialised bytes differ from what is in flash (a shadow copy
loaded at boot). No commit when nothing differs. `aoip_nvs_writes` counts
commits, `aoip_nvs_skipped` quiet periods that found nothing new. On an NVS
error the change stays dirty and is retried.

During a write the USB side keeps running bit-exact: its interrupt path is
IRAM-only (A2, `tools/check_iram.py`). The network side (EMAC RX task,
packetizer, lwIP) runs from flash and pauses for the write (ms; tens of ms
if NVS has to erase a page): RX frames wait in the 64-descriptor DMA ring,
TX packets due meanwhile go out late or their flow re-anchors
(`aoip_tx_lag_resets`). Coalescing makes this a rare, controller-initiated
event, typically once per patch/rename session. The record sizes depend on
`UI_CHANNELS`: flashing a different channel-count build starts from
defaults.

## 6. Test builds

- `CONFIG_USBINF_TEST_TONE`: TX channel N = −20 dBFS sine at N × 100 Hz,
  from a 480-entry table indexed by (frame × N) mod 480 — phase-continuous
  in the media frame index, identical on every flow carrying the channel.
- `CONFIG_USBINF_TEST_NET_LOOPBACK`: TX channel N = RX channel N, read from
  `g_ring_net_to_usb` at `ts − UI_NET_LATENCY_FRAMES` without clearing.
  **Requires that USB IN does not read-and-clear `g_ring_net_to_usb` in this
  build** (otherwise it races the TX reader). Stale data repeats every ring
  lap (10.7 ms) after a flow stops; acceptable for a bench test. TX peaks =
  RX peaks.

## 7. Deviations from Inferno / unverified

- **Device rename** (ARC 0x1001, content = name + NUL, empty = default) is
  not in Inferno; taken from network-audio-controller. Unverified against
  Dante Controller. Names limited to `[A-Za-z0-9-]`, ≤ 31 chars.
- Empty channel name in a rename = back to the factory name (Inferno would
  store "").
- Sample-rate / latency / encoding changes: unknown opcodes (ARC 0x1101,
  info 0x81, …) get no reply, as Inferno. Board info flags advertise no
  sample-rate configuration.
- Clock stats: leader id = EUI-64 of the PTPv1 leader UUID (Inferno reads an
  8-byte id from its clock daemon; layout assumed).
- Heartbeat 0x8001/0x8003 are always present (our clock always exists);
  Inferno omits them without a clock.
- Multicast TX: no mDNS address-conflict check / in-addr reservation during
  the grace period (TODO); the activated address is persisted (Inferno
  FIXME).
- A flow request with an invalid channel is refused (Inferno replies 0x0302
  and adds it anyway). Same-destination request applies the new fpp/encoding
  (Inferno keeps the old ones).
- TX send failure: next_ts still advances (Inferno retries the same packet).
- Flow "update" (0x0102) to reuse free slots is used as Inferno does; the
  P4 reference found a virtual soundcard sends silence in added slots after
  0x0102 — watch for this in M4.
- mDNS: our own TX channels cannot be resolved by our own subscriber
  (self-patch unsupported). espressif/mdns may rename the hostname on a
  conflict; our ARC name would then differ from mDNS.
- Peaks/latency counters are written racily (benign: one lost update).
- NVS writes still pause the network side (see §5); USB is unaffected.
- A TX rename to a name another TX channel uses (factory or friendly) is
  refused (it would make mDNS drop the other channel's service).

## 8. Test plan

Tools: `tools/aoip_probe.py <ip> [info|tx|rx|flows|subscribe|unsubscribe|rename-*]`
(ARC/CMC client), telemetry `aoip_*` keys on UDP 7779, Wireshark.
Peers: Dante Controller (Windows/macOS) and Inferno on Linux
(`inferno2pipe` / ALSA plugin, `RX_CHANNELS/TX_CHANNELS`, 48 kHz), one PTPv1
leader (a Dante device or Inferno's clock).

**M3 — discovery and control (no audio).**
1. Boot; `aoip_probe.py <ip> info`: CMC id = EUI-64 of MAC, 32/32, flags 0x30.
2. Dante Controller lists `UI3232-xxxxxx`, 32 TX / 32 RX named 01..32,
   48 kHz 24 bit, sync green once PTP locks.
3. Rename device, one TX and one RX channel in DC; reboot; names persist;
   `_netaudio-chan` shows the friendly alias (`avahi-browse -r _netaudio-chan._udp`).
4. Info requests: DC device view (network, clock, versions) populated.

**M4 — RX (Inferno/Dante → us → USB IN).**
1. Inferno on Linux transmits 8 ch; patch to RX 1–8 in DC: status goes
   in progress → green in < 5 s; `aoip_probe.py rx` shows RECEIVING;
   `aoip_rx_packets` climbs, `aoip_rx_late` ≈ 0, `aoip_rx_ka_sent` climbs.
2. Patch 32 channels from 4 transmitters; check 4+ flows (`flows`),
   0 wrong length, latency tab in DC shows values.
3. Reboot the transmitter (new link-local IP): patch recovers after ~2–9 s.
4. Reboot the device: subscriptions restore without user action.
5. Multicast: create a multicast flow on a Dante TX; subscribe; status
   multicast green.
6. Unsubscribe: flow stopped at the transmitter (its flow list empty).

**M5 — TX (USB OUT → us → Inferno/Dante).**
1. `CONFIG_USBINF_TEST_TONE` build: subscribe Inferno RX 1–8 to our 01–08;
   record; verify N×100 Hz at −20 dBFS, no clicks over 10 min (pcap through
   `tools/aoip_pcap.py`: timestamps: no gaps, fpp constant).
2. 32 channels to two receivers (unicast) and one multicast bundle; check
   ~12 k pps, `aoip_tx_send_fail` = 0, `aoip_tx_lag_resets` flat after start.
3. Stop the receiver without unsubscribing: flow expires after 4 s
   (`tx_flows` drops).
4. Normal build: play bit-exact test pattern from the USB host, capture the
   Inferno output, compare (24-bit transparent, fixed latency).
5. `CONFIG_USBINF_TEST_NET_LOOPBACK`: RX 1–32 from Inferno back to Inferno;
   compare signals and measure round-trip latency.
