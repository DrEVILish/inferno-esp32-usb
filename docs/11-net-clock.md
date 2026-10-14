# 11 — Network core and media clock

Code: `firmware/main/net/` (Ethernet, receive fast path, raw transmit,
multicast) and `firmware/main/clock/` (1588 counter, PTPv1 follower, servo).
Host tools: `tools/net_stats.py` (telemetry, `watch` mode) and
`tools/ptp_servo_sim.c` (servo simulation).

Everything below runs without hardware except where the
**unverified** list at the end says otherwise.

## 1. Board and pins (Waveshare ESP32-P4-ETH)

| signal | GPIO | notes |
|---|---|---|
| RMII REF_CLK (in) | 50 | 50 MHz external input, `EMAC_CLK_EXT_IN` (Waveshare: the IP101 50M_CLKO, PHY on its own 25 MHz crystal; rev 1 board: a 50 MHz oscillator, docs/40 §7.2); the APLL is not used |
| TX_EN | 49 | |
| TXD0 / TXD1 | 34 / 35 | |
| CRS_DV | 28 | |
| RXD0 / RXD1 | 29 / 30 | |
| MDC / MDIO | 31 / 52 | |
| PHY reset / power | 51 | pulsed low then held high by the PHY driver's hardware reset |
| PHY address | 1 | IP101 |

The data pins agree with IDF v6.1's `ETH_ESP32_EMAC_DEFAULT_CONFIG()` for
the P4 (the ESP32-P4-Function-EV-Board layout that the Waveshare board
copies). They also match the pin set the reference P4 receiver reports for
this board (28–31, 34, 35, 49, 50, 52). `app_config.h` was left unchanged.

GPIO51 has conflicting reports. Board configurations for the Waveshare P4
boards give it as the PHY power or reset pin, but the reference receiver ran
with no PHY reset (`-1`, "not wired"). Driving it as an active-low reset is
safe in every case: it resets the PHY, power-cycles it, or does nothing.
Check on the first bench run that nothing else on the board uses GPIO51.

## 2. Ethernet bring-up (`net_start`)

- **MAC:** `esp_eth_mac_new_esp32()`, with the RX task at `UI_PRIO_EMAC_RX`
  and 6 KB of stack, because hooks (audio decode) run on it.
  `ETH_MAC_FLAG_PIN_TO_CORE` pins that task to the core that installs the
  driver. `net_start()` therefore runs the install on `UI_CORE_NET` (core 0),
  through a short-lived pinned task if it is called from another core.
- **PHY:** IDF v6 moved the chip PHY drivers (IP101, LAN87xx, …) out of the
  core into the component registry. We use the core's generic IEEE 802.3
  driver (`esp_eth_phy_new_generic`). It handles reset, autonegotiation and
  link/speed/duplex from the standard registers (BMCR/BMSR/ANAR/ANLPAR),
  which is everything a 100 Mbit full-duplex IP101 link needs. This means no
  registry download and no network access at build time. If an IP101 quirk
  ever turns up, switch to `espressif/ip101` in `idf_component.yml`; it is a
  one-line change in `net.c`.
- **EEE off.** The EEE advertisement (MMD 7 register 60, through clause-22
  registers 13/14) is cleared, then autonegotiation is restarted. With
  802.3az, a lone Sync pays a link wake-up of tens of µs, but a Sync inside
  audio traffic does not. The path delay would then depend on load.
- **No 802.3x flow control** (`ETH_CMD_S_FLOW_CTRL` false). A receive stall
  must never pause the switch port.
- **DMA "hold" instead of "flush".** `DMAOPERATION_MODE.DFF` = 1: when no RX
  descriptor is free, the DMA holds the frame instead of flushing it. This
  was measured on this exact board. Flushing during a line-rate burst
  wedged the MTL RX FIFO read controller on a timestamped frame (EMACDEBUG
  read controller stuck in "frame status/timestamp", FIFO full), and only a
  MAC reset recovered it. This is applied at install and again on every
  link-up.
- **Pass-all-multicast** (`ETH_CMD_S_ALL_MULTICAST`). The EMAC has only a
  handful of perfect-filter slots, and lwIP asks for one per IGMP group.
  The slots run out quickly (PTP, mDNS, info, heartbeat, audio flows). We
  take every multicast frame and filter in software (see §3). lwIP's
  per-group MAC filter callback is replaced by a no-op.
- **esp_netif:** default Ethernet netif, DHCP client. With
  `CONFIG_LWIP_AUTOIP` + `LWIP_DHCP_AUTOIP_COOP`, it falls back to a
  169.254/16 address after `CONFIG_LWIP_AUTOIP_TRIES` (2) DISCOVERs.
  `net_ipv4()` reads lwIP's address directly, because a link-local address
  never posts `IP_EVENT_ETH_GOT_IP`. The hostname is `UI3232-xxxxxx`.
- **IEEE 1588:** see §5.
- **Maintenance task** (`net_maint`, core 1, `UI_PRIO_CONTROL`). Every second
  and on every link-up / got-IP, it re-joins in lwIP any wanted multicast
  group that lwIP has lost. A link loss makes esp_netif take the interface
  down, and lwIP frees every IGMP membership without re-joining. It also logs
  address changes.

`sdkconfig.defaults` sets: `CONFIG_ETH_TRANSMIT_MUTEX=y`, which is
**required** because lwIP (core 1), the TX packetizer and PTP (core 0) all
write the same TX descriptor ring. It also sets `CONFIG_ETH_IRAM_OPTIMIZATION=y`,
AutoIP, and SPI Ethernet and openeth off. `CONFIG_ETH_PHY_INTERFACE_RMII` is
gone in v6.1; RMII is selected in code. IGMP is always on in IDF's lwIP.

**DMA rings: 64 RX + 30 TX buffers of 1600 B** (A4, 2026-10-13). IDF v6.1
limits both counts to 3..30 in Kconfig (a larger value in
`sdkconfig.defaults` is ignored with only a NOTE). `firmware/components/esp_eth`
is a project-local copy of IDF's component (test_apps dropped) that shadows
the IDF one; its only change is the RX range 3..64
(`patches/esp_eth-rx64.patch`). Nothing in the driver is sized by a
narrower type (descriptors are chained; indices are `int`/`uint32_t`).
After an IDF upgrade run `tools/esp_eth_override.sh`, which re-copies the
component from `$IDF_PATH` and re-applies the patch. 64 descriptors cover
~2.7 ms at 24 k frames/s (1.3 ms at 48 k/s, the 0.25/0.5 ms latency
profiles) for 102 KB of internal DMA RAM. Bursts beyond that back up into
the MTL RX FIFO, because of the hold mode above, rather than being flushed.
The RX task (priority 23, core 0, hooks only doing decode) still has to keep
up on average.

## 3. Receive fast path (hooks)

`esp_netif_attach()` installs the glue's input function. Right after it,
`net_start()` replaces that function with `esp_eth_update_input_path_info(...,
rx_input, netif)`. The EMAC driver's RX task (core 0) calls `rx_input()` for
every frame, with the frame in a heap buffer it allocated. It also passes
`info`, the hardware RX timestamp (`eth_mac_time_t`, zero if none).

`rx_input()`:

1. Ethernet type 0x0800, IPv4 with no fragment (MF = 0, offset = 0), protocol
   UDP, and the lengths consistent. Anything else goes to lwIP untouched.
2. Destination port matched against the hook table (16 ranges, lock-free
   reads, first match decides). On a match the hook runs with the UDP
   payload and `net_udp_meta_t`, which carries the hardware timestamp in ns
   when there is one. If the hook returns true, the buffer is freed
   (`rx_hooked`). If it returns false, the frame continues to lwIP.
3. **Multicast filter:** a 239.0.0.0/8 frame (AoIP audio) to a group that is
   not in `net_mcast_join()`'s table is dropped (`rx_mcast_filtered`). Without
   IGMP snooping, every other device's multicast flows would otherwise flood
   lwIP's tcpip mailbox. Other multicast (224.0.0.x, PTP 224.0.1.129) passes.
   **Rule for aoip/:** join 239/8 groups with `net_mcast_join()`, not only with
   a socket's `IP_ADD_MEMBERSHIP`.
4. Everything else: `esp_netif_receive()` (`rx_to_lwip`).

Cost per hooked frame: a header parse (~20 loads) and a free(). The copy out
of the DMA buffer and the malloc are IDF's (`emac_esp_dma_alloc_recv_buf`).
24 k frames/s is about 24 k malloc/free pairs/s on core 0, which the
reference receiver showed is fine at its rates. If profiling says otherwise,
the next step is a private RX loop, which needs a patched driver.

Hooks must not block or call lwIP. lwIP is fed from this same task, so a
blocking call into it can deadlock.

## 4. Raw UDP transmit

`net_udp_dest_resolve()`:
- Multicast maps to `01:00:5e` plus the low 23 bits. Broadcast maps to
  `ff:…`. Both resolve immediately.
- Unicast is looked up with `etharp_find_addr()` in the tcpip thread
  (`esp_netif_tcpip_exec`). For off-subnet, non-link-local destinations the
  gateway is looked up instead. If the address is missing, `etharp_query()`
  sends a request and the call returns `ESP_ERR_NOT_FINISHED` at once; retry
  later. Do not call it from a hook. Re-resolve from time to time, because
  ARP entries age out.

`net_udp_send_raw()` builds the 42-byte Ethernet/IPv4/UDP header on the
stack:
- DSCP from the destination, an incrementing IP id, DF set
- TTL 64 for unicast, 32 for multicast
- IP header checksum computed; UDP checksum 0 (legal for IPv4)

It then hands header and payload to `esp_eth_transmit_ctrl_vargs()` as two
buffers. The driver copies both into the DMA buffer under the TX mutex. There
is no heap allocation and no extra copy of the payload. Failures (link down,
no free TX descriptor, mutex timeout) return the error and count in
`tx_raw_fail`. The source IP is the current lwIP address, 0.0.0.0 before
there is one.

`net_udp_send_raw_ts()` (internal, PTP only):
1. Waits (≤ 1 ms) for the TX DMA to be suspended, so the frame is not queued
   behind audio.
2. Sets TDES0.TTSE (bit 25) for the next frame through
   `ETH_MAC_ESP_CMD_SET_TDES0_CFG_BITS`.
3. Sends with a `eth_mac_time_t` control argument, so the driver polls the
   descriptor for the TX timestamp.
4. Clears TTSE again.

IDF's PTP enable sets TTSE for *all* frames. We clear it right after enable,
so only Delay_Req is stamped. The driver's poll is bounded (50 iterations,
measured ~31 for a 1500 B frame). A frame stuck behind a deep TX queue would
time out and return 0 (`ptp_no_hw_tx_ts`), which the idle wait prevents.

## 5. IEEE 1588 counter (`mediaclock.c`)

Enabled in `net_start()` with `esp_eth_mac_ptp_enable()`:

- **Clock source:** XTAL, 40 MHz (25 ns).
- **Resolution:** sub-second increment 40 ns, digital rollover (TSCTRLSSR),
  so the nanoseconds register counts real ns (0…999 999 999).
- **Update method:** fine (addend). Nominal addend = 2³² × 25/40 =
  0xA0000000, read back from the register at `mc_start()`.

PLL_F80M (12.5 ns, 20 ns increment) would halve the resolution. XTAL is
IDF's default and certainly running. 40 ns quantisation averages out in the
servo (simulated: ~40 ns rms).

Then the timestamp classifier is reprogrammed for **PTPv1 event messages over
UDP/IPv4 only**:

| field | value |
|---|---|
| TSENALL | 0 |
| TSVER2ENA | 0 (v1) |
| TSIPENA | 0 (no PTP-over-Ethernet) |
| TSIPV4ENA | 1 |
| TSIPV6ENA | 0 |
| TSEVNTENA | 1 (event messages) |
| TSMSTRENA | 0 (slave) |
| SNAPTYPSEL | 0 |
| TSENMACADDR | 0 |

On receive this stamps Sync only. **Never TSENALL:** stamping every frame was
the other half of the RX FIFO hang above.

Register use:

- **`mc_now_ns()`** (IRAM, any core, any context). It reads `sys_seconds`,
  `sys_nanosec`, `sys_seconds` again, and re-reads the nanoseconds if the
  seconds changed (one rollover at most), then returns `s × 1e9 + ns`. That
  is three or four APB reads, with no lock. Before the counter is
  initialised it returns `esp_timer × 1000 + 1 s`. The counter is then
  started from that same value, so time is continuous.
- **Step** (`mc_hw_step`). In a critical section: read the time, add the
  delta, write `sys_seconds_update` / `sys_nanosec_update`, then set TSINIT
  and wait for it to clear. The epoch is incremented inside the same
  critical section, and both are bracketed by a sequence counter, so
  `mc_now_ns_epoch()` (IRAM; used by the USB SOF ISR and the packetizer)
  always returns a time and the epoch of *its* timescale. This is an absolute re-initialisation, not
  TSUPDT add/subtract. The subtract encoding of the update register differs
  between GMAC versions, while TSINIT is unambiguous. The read-to-write gap
  is well under 1 µs and the servo absorbs it. Every step increments
  `mc_epoch()`. Time is kept ≥ 1 s, because the driver reports a zero
  timestamp as "none".
- **Frequency** (`mc_hw_set_freq_ppb`). `addend = nominal × (1 + ppb·1e-9)`,
  written to TSAR and latched with TSADDREG once the previous update has
  completed. One addend LSB is ≈ 0.37 ppb. `mc_freq_ppb()` returns the
  applied value (positive = counter sped up = crystal slow).

Before PTP lock the counter free-runs on the crystal. USB and the media clock
work unchanged.

## 6. PTPv1 follower (`ptpv1.c`)

PTPv1 is IEEE 1588-2002: UDP 319 (event) and 320 (general) on 224.0.1.129,
subdomain `_DFLT`. 224.0.1.130–132 (`_ALT1`–`_ALT3`) are not used by AoIP
networks and are not joined. The device is a follower only and never sends
Sync.

The hooks on 319/320, in the EMAC RX task:
- Check `versionPTP = 1` and `versionNetwork = 1`. A PTPv2 frame has
  versionPTP 2 in byte 1 and is ignored.
- Check the subdomain.
- Parse the message into a ≤ 64-byte event and `xQueueSend(…, 0)` it to the
  PTP task (queue 16, drops counted). The PTP task runs on core 0 at
  `UI_PRIO_PTP` = 20, below the TX packetizer (22): its Delay_Req path
  busy-waits for the TX DMA. Each event carries `mc_epoch()` from the hook;
  a Sync stamped before a step is dropped (it would read as an offset of
  the whole step).
- Consume every 319/320 frame.

Header offsets used (bytes): versionPTP 0, versionNetwork 2, subdomain 4–19,
messageType 20, sourceCommunicationTechnology 21, sourceUuid 22–27,
sourcePortId 28, sequenceId 30, control 32, flags 34 (PTP_ASSIST = 0x0008).
Message fields:

| message (control) | port | fields used |
|---|---|---|
| Sync (0), 124 B | 319 | originTimestamp 40 (u32 s, i32 ns), grandmasterClockUuid 54, grandmasterPortId 60, grandmasterClockStratum 67, grandmasterClockIdentifier 68, grandmasterClockVariance 74, grandmasterPreferred 77, grandmasterIsBoundaryClock 79, syncInterval 83, localStepsRemoved 90; HW RX timestamp = t2 |
| Follow_Up (2), 52 B | 320 | associatedSequenceId 42, preciseOriginTimestamp 44 = t1 |
| Delay_Resp (3), 60 B | 320 | delayReceiptTimestamp 40 = t4, requestingSourceUuid 50, requestingSourcePortId 56, requestingSourceSequenceId 58 |

**Leader selection.** Every Sync sender is a candidate (4 slots, forgotten
after 3 s of silence). A new candidate needs 2 Syncs before it can win. The
comparison follows the 2002 data set comparison:
- Same grandmaster: fewer steps removed wins, then the lower sender UUID.
- Different grandmasters, in order: preferred first, then lower stratum,
  then identifier (byte order), then lower variance, then the lower
  grandmaster UUID.

In an AoIP network normally only the elected leader sends Sync, so this only
decides during handovers. The current leader is lost after 3 s without a Sync.
The servo then keeps its frequency (holdover), forgets path delay and lock,
and re-acquires on the next leader. It steps only if that leader's time is
more than 100 µs away, which also bumps the epoch.

**Two-step.** For a Sync with PTP_ASSIST, (seq, t2) is kept until the
matching Follow_Up from the same sender supplies t1. A Sync without the flag
uses its own originTimestamp. A Sync from the leader without a hardware
timestamp is counted (`ptp_no_hw_rx_ts`) and skipped.

**Delay_Req.** This is a 124-byte Sync-format message from our UUID (the
EMAC MAC), port 1, to 224.0.1.129:319, DSCP CS7, UDP source port 319. Its
data set fields echo the leader's, with local stratum 255 and identifier
`DFLT`. It is sent only once the servo is tracking: Delay_Req samples taken
during the frequency error are biased. It goes out every 250 ms until 16
path delay samples are in, then every 1 s ± 25 %. Its hardware TX timestamp
is t3. The Delay_Resp that matches our UUID, port and sequence gives t4.
Requests unanswered after 1.5 s are dropped. The leader answers by
multicast, so no IP address is needed. Sending starts on link-up.

**After a step,** the pending Sync and any outstanding Delay_Req are
discarded. Their timestamps belong to the old timescale.

## 7. Servo (`ptp_servo.c`, pure C)

Inputs are `ms = t2 − t1` per Sync and `sm = t4 − t3` per Delay_Resp.
Path delay `d = (ms + sm)/2`, and offset `= ms − d` (local minus leader).

Stages:

1. **IDLE → step.** The first sample steps the clock by −offset. Before
   that, time is unrelated to the leader. Path delay is 0 if unknown.
2. **FREQ.** Collect (t2, ms) for ≥ 6 samples spanning ≥ 3 s (at most 32).
   The slope is a **Theil–Sen** estimate: the median of pairwise slopes of
   points ≥ 1 s apart, so a few queueing spikes cannot bias it. The
   frequency is then set to `current − slope·1e9`. The next sample (which
   straddles the rate change) is skipped.
3. **TRACK.** PI on the offset, in ppb: `integral −= Ki·off·dt`,
   `freq = integral − Kp·off`, with dt from consecutive t2.

| constant | value | meaning |
|---|---|---|
| `SERVO_STEP_NS` | 100 µs | \|offset\| above this: step (epoch++) |
| `SERVO_SNAP_NS` | 20 µs | one phase snap when the path delay first becomes known |
| `SERVO_KP_ACQ`, `SERVO_KI_ACQ` | 0.5 /s, 0.0625 /s² | acquisition (τ ≈ 2 s, critically damped: Ki = Kp²/4) |
| `SERVO_KP_LOCK`, `SERVO_KI_LOCK` | 0.15 /s, 0.005625 /s² | locked (τ ≈ 7 s) |
| `SERVO_MAX_PPB` | ±300 ppm | correction clamp |
| `SERVO_DELAY_N` | 16 | path delay: **25th percentile** of the last 16 samples (valid from 3) |
| `SERVO_OUTLIER_NS` | 5 µs | locked: a sample this far from the last accepted offset is held out |
| `SERVO_SHIFT_N`, `SERVO_SHIFT_AGREE_NS` | 40, 8 µs | 40 consecutive outliers within 8 µs of each other = a real phase shift: accepted and slewed (stepped only if > 100 µs) |
| `SERVO_HOLD_MAX` | 80 | scattered outliers for 20 s: unlock, re-acquire |

- **On a step** the proportional term is dropped (frequency := integral), so
  a saturated P term cannot carry over.
- **Path delay** uses a low percentile, not the median, because queueing
  only ever *adds* delay. Sync bursts that arrive late (seen on real
  networks: +9…19 µs for 4–7 s every ~89 s) then do not move it. A
  Delay_Req whose paired Sync was an outlier is discarded.
- **During outliers** the rate is held (holdover on the integral) and the
  reported offset is the last accepted one.

**Lock:** 8 consecutive samples with \|offset\| < 3 µs and path delay known.
**Unlock:** 8 consecutive accepted samples beyond ±10 µs, a step, 80
scattered outliers, or leader loss. The hysteresis follows bench experience
that 2 µs / 5 µs thresholds flap on PTPv1, which has no correction field, so
switch queueing appears directly as offset jitter.

**Simulation** (`tools/ptp_servo_sim.c`, 300 s, Sync 4 Hz, 50 µs path, 40 ns
timestamp quantum, +41/+80 ppm crystal):

| scenario | lock after first Sync | locked offset (true) |
|---|---|---|
| 0 clean | 7.6 s | 39 ns rms |
| 1 jitter + periodic +9…19 µs Sync bursts | 7.6 s | 94 ns rms, p99 200 ns, 0 lock losses |
| 2 + persistent −36 µs leader→us shift at 60 s | 7.6 s | p99 1.1 µs after re-settling (settles at the +18 µs that v1's symmetry assumption implies) |
| 3 = 2 with +80 ppm crystal | 7.6 s | p99 0.24 µs |

## 8. Telemetry

`sys/telem.c` exports `ptp_locked`, `ptp_epoch`, `ptp_offset_ns`,
`ptp_freq_ppb`, `ptp_leader`, `ptp_stage`, `ptp_path_delay_ns` and counters:
`ptp_syncs`, `ptp_follow_ups`, `ptp_delay_reqs`, `ptp_delay_resps`,
`ptp_no_hw_rx_ts`, `ptp_no_hw_tx_ts`, `ptp_queue_drops`, `ptp_steps`,
`ptp_lock_losses`, `ptp_outliers`, `ptp_phase_shifts`, `ptp_leader_changes`,
`ptp_delay_samples`, `ptp_delay_rejected`. It also exports `net_*`,
including `net_rx_mcast_filtered` and `net_ipv4`.

```
tools/net_stats.py 169.254.x.y                 # all keys
tools/net_stats.py 169.254.x.y watch 120 --csv run.csv
```

Logging is rate-limited, to at most one PTP log line per second (leader
change, lock/unlock, step), and nothing is logged per packet.

## 9. Unverified without hardware

1. That the PTP classifier stamps PTPv1 Sync over IPv4 with these bits, and
   that the descriptor RX timestamp is present on exactly those frames. It
   is consistent with the Synopsys GMAC databook and with the reference
   receiver's working configuration. Check: `ptp_no_hw_rx_ts` stays 0.
2. TX timestamps for Delay_Req via TTSE plus the driver's bounded poll,
   under audio TX load. Check: `ptp_no_hw_tx_ts` stays 0 with 4 × 8-channel
   flows running.
3. TSINIT stepping while the counter runs (no lost seconds, no glitch other
   than the step).
4. Digital rollover with the XTAL source: the counter advances 1 s/s
   (compare against `uptime_ms` before lock).
5. GPIO51 behaviour (§1), and that the generic PHY driver reports 100/full on
   the IP101.
6. The EEE register write on the IP101. A PHY without EEE ignores it. Check
   the partner's view, or the absence of an idle-vs-load delay change.
7. The DMA hold bit surviving link down/up (it is re-applied on link-up).
8. The BMC ordering among *different* grandmasters. In practice there is one
   Sync sender.
9. PTP_ASSIST bit position (0x0008 in `flags`). If a leader were
   two-step without the flag, offsets would show the Sync's approximate
   origin error. Check: `ptp_follow_ups` ≈ `ptp_syncs`, and offsets are
   sub-µs.
10. Throughput of the RX path at 24 k frames/s (48 k at the low-latency
    profiles) with IDF's per-frame malloc/copy and 64 RX descriptors.
11. IGMP joins issued before an address exists (the source is 0.0.0.0). These
    are allowed for snooping switches. lwIP re-reports groups when the
    address arrives.

## 10. Test plan for M2 (clock)

Setup: the board and an AoIP leader (hardware device or DVS) on one switch,
first unmanaged, then managed with IGMP snooping. Run
`tools/net_stats.py <ip> watch 600 --csv m2.csv` from power-on.

| # | test | pass |
|---|---|---|
| 1 | Power-on to `ptp_locked=1` | < 25 s (link ~3 s, link-local address ≤ 10 s, but PTP starts at link-up; simulated lock 7.6 s after the first Sync) |
| 2 | Locked offset over 10 min, idle network | \|offset\| typically < 1 µs, p95 < 2 µs; `ptp_steps` and `ptp_lock_losses` flat after lock |
| 3 | `ptp_freq_ppb` | stable to ±50 ppb after 2 min; equals the crystal error (tens of ppm) |
| 4 | Start/stop 4 audio flows each way | no lock loss; `ptp_no_hw_tx_ts`, `ptp_no_hw_rx_ts`, `ptp_queue_drops` stay 0; at most 1 `ptp_phase_shifts` per path change |
| 5 | Leader handover (power-cycle the leader, or change the preferred leader) | `ptp_leader` changes, re-lock < 25 s, `ptp_epoch` +1 if the new timescale differs |
| 6 | Pull the cable 10 s, re-plug | groups re-joined (Sync count resumes), re-lock < 25 s, no step if the leader is unchanged and the holdover drift < 100 µs |
| 7 | Managed switch with IGMP snooping, leader on another switch | Syncs arrive (the join works) |
| 8 | Cross-check against the leader | PPS out (`esp_eth_mac_set_pps_out_gpio`, bench build only) against the leader's PPS or a DVS timestamp: within ±1 µs, plus the expected v1 asymmetry |
| 9 | 1 h soak with audio | 0 RX FIFO hangs (`net_rx_frames` keeps rising), `net_tx_raw_fail` ≈ 0 |
