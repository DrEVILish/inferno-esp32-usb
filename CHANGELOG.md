# Changelog

All notable changes after the initial import. Format loosely based on Keep
a Changelog; "verifiable" = the claim is covered by a host test or
deterministic review, "bench" = still gated on hardware.

## [0.1.4] — 2026-10-14: regression audit of 0.1.3 (three induced issues found)

Deep re-review of the eight-finding fix round with reliability /
performance / Inferno-compatibility lenses. Three issues introduced by
0.1.3 were found and fixed here:

- **Inferno compat (serious):** the channel-id TXT validation added in
  0.1.3 demanded `1 ≤ id ≤ nchan` — but the fork's `nchan` TXT is
  *channels per flow* (`MAX_CHANNELS_IN_FLOW.min(channels)` in
  mdns_server.rs), not the device channel count: a 32-channel device
  advertises `nchan=8` while the per-channel ids legitimately run 1..32,
  and its client parses `id` unbound. The bound would have made every
  subscription to channels 9..32 of any 32-channel Inferno transmitter
  fail to resolve. Removed; the intended protection (id must not wrap)
  is provided by the nonzero + u16-range checks that stay. The host test
  now pins `id=9` (nchan 8) as VALID and `id=65537` as refused.
- **Inferno compat:** `latency_ns` parsing lost the fork's `0x` hex
  prefix support (`parse_int_from_dict` accepts hex for all ints);
  restored. Host-tested with a `0x1e8480` vector.
- **Reliability:** the uncollected-search pool in the new bounded mDNS
  resolve was sized for two phases (2×CH) while one full cycle can park
  three phases' worth (SRV+TXT of all channels + one A query per host).
  On a fully dead network every cycle parked 32 phase-2 handles beyond
  the pool and leaked one search handle each, repeating. Pool sized
  3×CH, matching the full-cycle bound (the drain-at-start still empties
  it before the next cycle adds).

Verified clean in the same audit: the ARC 0x3010/0x3014 TODO-ERROR reply
only fires when a command cannot be enqueued (bounded 250 ms wait inside
the held `aoip_lock` resolves bounded: queue pops need no lock); the
enqueue-first state publish keeps the Dante "status already subscribed"
ordering; `ctl_ensure_started()` runs off the shared lock and its retry
cannot double-open sockets or double-start the (idempotent) subscriber;
the bounded resolve finishes in ~7.5 s worst case with all 2·CH+CH
parked handles reaped; parser strictness beyond the fork (nonzero dbcp1/
fpp, depth validation at parse) only changes outcomes on records the
fork itself would fail downstream anyway. Performance: no hot-path cost
added (all changes are control-plane or compile-time).

## [0.1.3] — 2026-10-14: review-round 2 fixes (8 findings)

All findings from the second external review round. "Fixed" items carry a
verification; the two design-constraint items get documentation parity.

1. **Fixed · AoIP TXT values wrap before validation (subscribes the wrong
   channel):** the mDNS chan/bundle TXT parser narrowed parsed integers
   straight into 16-bit fields — `id=65537` became channel 1 and got
   subscribed as if the transmitter advertised it. The parsing split out of
   `aoip_mdns.c` into `aoip/mdns_txt.c|h` (host-testable), every integer is
   now range-checked against its protocol limit *before* the narrowing
   (ints ≤ u16, `1 ≤ id ≤ nchan`, depth 16..32 byte-aligned, fpp pair
   nonzero and ≤ u16, bundle address/port present and nonzero), and the
   subscriber's own post-checks stay as belt-and-braces. New host test
   `tools/mdns_txt_test.c` (chan + bundle vectors, wrap cases, valueless
   keys) in `tools/host_tests.sh`.
2. **Fixed · A full command queue silently lost subscription changes:**
   `sub_subscribe()` published "subscribed" state before the nonblocking
   enqueue; a full queue left a state nothing would reconcile (and a
   dropped unsubscribe left the stream active). Queueing now happens first
   (one bounded 250 ms wait against a busy resolve), visible state is
   published only on success, and `sub_subscribe/sub_unsubscribe` return
   bool: ARC 0x3010/0x3014 reply TODO ERROR when a command could not even
   be enqueued. The `resolve` hard deadline (see 4) bounds the queue-full
   window itself.
3. **Fixed · Socket / subscriber startup failures ran half a device
   silently:** the four server sockets and the subscriber task now start
   through `ctl_ensure_started()` — bind/task failures are retried every
   10 s from the control loop, transitions are logged once, and the bits
   (arc/cmc/info/fcs/sub) surface in telemetry as `aoip_start_degraded`
   (`aoip.h` new field, `tools/net_stats.py watch` shows `0x0` when whole).
   `sub_start()` became idempotent and retryable (queue kept, no
   double-init).
4. **Fixed · mDNS fallback lookups blocked the subscriber task ~96 s:**
   the serial per-host 3 s `mdns_query_a` fallback is gone. All channels'
   SRV+TXT queries now start at once (staggered like the Inferno client)
   and gather under a hard 4.2 s deadline; a second parallel round for the
   distinct hosts whose SRV answer carried no IPv4 address gathers under
   its own 2.8 s deadline; searches that outlive their window are parked
   and reaped at the start of the next cycle (no handle leaks). Worst case
   per resolve is now ~7 s instead of ~96 s.
5. **Fixed · Cached host addresses never expired:** the 8-entry host→IP
   cache carries a timestamp and entries expire after 2 minutes (a peer
   that changes address is picked up then). Names that are longer than the
   cache key are not stored at all — a truncated entry could previously
   have been served for a different host with the same 31-byte prefix.
6. **Documented · No authentication on the control plane (design
   constraint):** the README threat-model note now states the concrete
   consequences (re-route RX, rename, create/stop TX flows, control-task
   load) and the mitigations (managed switch, ACLs, IGMP snooping); an
   auth layer would be protocol-divergent and is out of scope.
7. **Documented · Firmware not yet exercised on hardware (validation
   gap):** README status section now says explicitly that all UX/latency
   figures are claims until the M1–M8 bench matrix has passed.
8. **Fixed · Profiling probes always in the hot path:** the t_* probes are
   gated by new Kconfig `USBINF_PERF_PROBES` (default y while the bench
   matrix runs). With it off, `perf_begin/end` compile to no-ops and the
   `t_*` telemetry keys stop appearing; documented in `docs/15 §1` for the
   post-bench production image.

## [0.1.2] — 2026-10-14: second full review round (5 dimensions)

Fresh review of the whole tree (bugs/edge cases, performance, security,
style, error handling). Two Warning findings fixed:

- **info_mcast.c (security):** the info-request token bucket's clamp broke
  whenever one idle gap longer than ~4 s preceded a refill burst —
  `INFO_TOKENS_MAX - refill` underflows for `refill >= 64`, so the "else"
  branch added tokens uncapped and every long idle period accumulated a
  possibility of a six-figure burst allocation. Net effect: the anti-flood
  limit silently disarmed itself after any quiet stretch, and the next
  request burst elicited an unbounded sequence of ~340-byte multicasts
  (line-rate amplification for a moment). Reworked with an always-clamped
  addition plus fractional-token carry (the previous code also shaved up
  to one token per refill under a steady drip, so the documented 16/s
  ceiling is now actually delivered).
- **net.c (edge case):** the RX fragment filter masked `0x3FFF`, which
  keeps the MF flag (0x2000) — a *first* fragment (MF set, offset 0) of a
  fragmented UDP datagram passed as "unfragmented" and was handed to the
  audio/keepalive hooks (its payload stops at the fragment boundary, its
  media header is truncated garbage) while the continuation fragments were
  dropped by lwIP instead of reassembled. Only continuation fragments had
  been rejected. The filter now requires the whole flags+fragment word
  (mask 0x7FFF) to be zero.

Review conclusions with no change needed (verified, for the record):

- uac2.c the arm paths hold: the OUT era guard cannot swallow a live
  transfer (it only moves inside the SET_INTERFACE critical section), and
  the wrap-to-collision of the u8 era takes 2^8 SET_INTERFACE cycles with
  a latched stale completion at exactly the wrap instant — noted as a
  backlog suggestion (widen to u16) rather than fixed.
- subscriber.c the alias/reuse/flow-slot logic is consistent; one edge is
  documented as backlog (command-queue-full leaves a channel stuck in
  status UNRESOLVED — pathological since the queue is 64 deep).
- settings_nvs.c the dirty/shadow/quiet-period policy is correct; two
  backlog suggestions recorded (unsupported version silently downgrades to
  defaults and later overwrites those records; persistent NVS failure
  retries every 2 s forever without backoff).
- flows_rx.c the hook/alive lives up to its comments ("active" handoff,
  map double-buffer, peaks races are by design); keepalives hitting a
  media port already die on the `len <= MEDIA_HEADER_LEN` check before
  any timestamp parse, but the hook never validates `MEDIA_BYTE0` — a
  stray ≥10-byte packet is merely counter noise (backlog suggestion).
- telemetry/read performance: the per-packet `esp_timer_get_time()` in the
  RX hook (~0.2 % of a core at 24 k pps) stays until the bench says
  otherwise; the unauthenticated telemetry socket is within the documented
  trusted-LAN threat model but is now listed as hardening backlog.

## [0.1.1] — 2026-10-14: regression-audit pass on 0.1.0

Deep re-review of everything below with three lenses: reliability,
performance, Inferno wire compatibility. **Four issues induced by 0.1.0
were found and fixed here:**

- **build break:** `eng_stage_in_packet`/`eng_commit_in_packet` had no
  prototypes in `usb/uac2_engine.h` (`uac2.c` called them; neither the host
  test suite nor the engine simulation exercises the split pair because it
  uses the combined `eng_in_packet`) — the firmware build would have failed
  at the bench.
- **latent NULL deref:** `net_udp_send_raw_ts()` called
  `net_udp_dest_seal()` before its argument checks; a NULL/invalid `const`
  dest (API-legal) crashed instead of returning `EINVAL`-style.
- **Inferno compat:** device-rename validation was one character too strict
  (30 vs the fork's 1–31).
- **Inferno compat (mitigation):** the new info-request rate limit started
  at 4 answers/s steady-state — thin for an unknown controller poll rate;
  rebalanced to 16/s (≈ 43 kbit/s worst-case multicast, burst 64) while
  keeping the line-rate flood ~99.9 % refused.

Verified clean (no changes needed) in the same audit: the monotonic ring
head cannot strand stale-appearing content — `reanchor_out()` already
silence-backfills from `now − ENG_OUT_BACKFILL` through `now + LEAD`
explicitly so nothing below head is stale; the OUT era guard cannot swallow
a live completion (era only moves inside the SET_INTERFACE critical
section); stage/commit re-render on a failed arm covers every frame it
rendered (commit clears `[r, r+n)` of the *succeeding* render); failed
clock steps keep seq even and epoch frozen, and the servo re-detects the
unchanged offset on the next Sync (step retry, no inflight loss).

## [0.1.0] — 2026-10-14: controller-surface + review fixes + instrumentation

### Inferno-fork controller surface (DrEVILish/inferno parity)

- ARC **0x2204** TX-flow labels: empty page (`02 00`), ends per-poll
  controller timeouts (fork 1491622). Byte-verified.
- ARC **0x1100** device settings: six read-only records — sample rate;
  default/configured/active latency = build-profile latency; max 10 ms;
  min 0.25 ms — with packet-relative value offsets (fork ee9eece + 5981fee
  form). ARC **0x1102**: 31-entry property directory verbatim. Both
  byte-verified against the fork sources; tables regenerable
  (`tools/check_vectors.py`-style comparisons done during dev).
- ARC **0x1101** latency writes: refused cleanly (`0x30`, empty) — latency
  is build-time here (divergence documented in docs/12 §7).
- Info-multicast: capability statuses **0x80** (rate) / **0x82** (encoding)
  announced at startup and answered on **0x81/0x83** probes, requested =
  current; clock status (**0x21** probe) migrated to the fork's 188-byte
  template (locked follower sync state 3, slave port states,
  EUI-64 identities) once the PTP leader is known — gated when it is not.
  All byte-identical to fork vectors (`tools/aoip_host_test.sh`, 12 checks).

### Review-derived fixes (docs/13 §6)

- **Clock (critical):** `mc_hw_step`/`mc_hw_set_freq_ppb` now return bool;
  a rejected TSINIT/addend write no longer advances epoch nor desyncs the
  servo; `ptp_hw_step_fails`/`ptp_hw_freq_fails` telemetry; PTP hook
  registration retried from `ptp_task` (was permanent-silent death).
- **Ring (reliability):** producer head is monotonic
  (`ring_publish_head`), preventing head regression from re-anchor writes.
- **USB (data loss):** IN path restructured stage→arm→commit — a failed
  `usbd_edpt_xfer` re-renders instead of silently discarding frames;
  gap fill skipped beyond `ENG_OUT_LEAD_MAX` (was unbounded in gap);
  stale completions across SET_INTERFACE swallowed via an arm era;
  endpoint alloc/activate refusals counted (`usb_ep_errors`) and logged;
  `uac2_start` retry can no longer double-init; GET controls guard
  `wLength`; unknown-EP completions not claimed.
- **Control plane (robustness/security):** flow-destination sanity
  (0.0.0.0/broadcast/loopback/port 0 → 0x0302) closes the reflection
  vector; `ftx_add_flow` failure codes disaggregated; info-request token
  bucket with a visible drop counter; multicast bundle removal only
  persists state after a successful flow removal; channel ids clamped at
  the `txm_add_flow` choke point; device rename validates per the fork.
- **Docs:** `docs/12` divergences updated; `docs/13` §5 (control-plane
  follow-ups) and §6 (fix table); README threat-model note.

### Performance (docs/15, Proposal 1)

- **Measurement:** IRAM-safe cycle-counter probes with windowed histograms
  on `rx_input`, `rx_hook`, `build_and_send`/`send`, and the USB
  SOF/xfer ISRs (`t_*_p50/p99/max/ovf` telemetry), plus free-RX-descriptor
  pressure (`net_rx_dma_free`) via a new getter in the local esp_eth
  override. Decision table for the next optimization round lives in
  `docs/15 §5`.
- **Applied micro-opts:** TX packet timestamps incremental (two 64-bit
  div/mod → one conditional carry per packet; 800k steps host-verified);
  per-destination 42-byte header template with checksum-by-linear-fold
  (200k sends verified equal to full rebuild via `tools/checksum_test.c`),
  sealed on resolve and refreshed on source-address change (ftx_tick
  republish + per-Delay_Req); 239/8 membership bitmap replacing a 32-slot
  atomic scan under multicast flood (bijectivity host-verified).
- **Bench-gated backlog (documented, not applied):** EMAC RX malloc/copy
  zero-copy path (the ~24k-pps ceiling suspect), peak metering out of the
  decode loop, packetizer wake economics, TCM/IRAM placement, `-O3`.

### Earlier (same release window)

- mDNS A-record fallback cache (hostname→IPv4, 8 entries): 32-Bench
  subscriptions to a badly-announcing host drop ~100 s → ~3 s.
- fpp clamp on flow requests (`aoip/fpp.h`): `fpp_min` above the MTU cap
  refuses the flow instead of requesting fragmenting silence; above the
  latency cap requests degraded-but-unfragmented (`tools/fpp_select_test.c`).
- `aoip_lock` scope: NVS flush and device-rename mDNS republish run after
  the ctl-loop unlock; socket/quiesce/IGMP work out of
  `frx_add/remove_flow`; `rebuild_map` unlocked.
- Host test runner: `tools/host_tests.sh` (fpp, checksum, perf windows,
  ts-split/bitmap over 800k steps, fork-vector ARC/info replies).

### Test status

Host tests + engine simulation all pass on every profile. The firmware
itself builds only with ESP-IDF (not present here) — the first bench build
is the outstanding gate; `tools/check_iram.py` gates every build.
