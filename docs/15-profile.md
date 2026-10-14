# 15 — Performance instrumentation (Proposal 1)

Status: instrumentation + no-regret micro-optimizations on main, 2026-10-14.
**Nothing is measured on hardware yet** — this file defines what to measure and
lists the optimizations that stay gated behind those measurements.

docs/01 §4 and docs/14 §6 estimate per-packet cost and packet-rate ceilings
with "(verify)" tags. This harness turns them into numbers: cycle histograms
on the hot paths, exported through the existing UDP telemetry, plus DMA-ring
pressure. Findings feed Proposals 2 (zero-copy EMAC DMA path) and 3
(placement/scheduler).

## 1. Probes (`firmware/main/sys/perf.h`)

`perf_begin()/perf_end()` bracket a region; the duration lands in an 8-cycle
histogram (32 buckets 0-255 cycles + overflow). Recording is a static-inline,
IRAM-safe pair (`check_iram.py` walks it from the USB ISR roots like all ISR
code) costing ~10 cycles per pair; histograms live in internal `.bss`
(~840 B). Windowed: `perf_report()` returns and resets since the last read.

| probe | measures | runs in |
|---|---|---|
| `rx_input` | whole EMAC RX fast path per frame (header parse, hook dispatch, lwIP handoff) | EMAC RX task, core 0 |
| `rx_hook` | audio decode per audio packet (`flows_rx.c`) | EMAC RX task, core 0 |
| `tx_build` | packet build per TX packet (`build_and_send`) | packetizer task, core 0 |
| `tx_send` | `net_udp_send_raw` (header + DMA handoff), nested in `tx_build` | packetizer task, core 0 |
| `usb_sof` | SOF ISR body per microframe | USB ISR, core 1 |
| `usb_xfer` | transfer-completion ISR body | USB ISR, core 1 |

Constraints: begin/end same core; a probe never nests within itself; each
probe ID lives in one execution context. Expected read-out overhead
(~1-3 % of one core at profile-2 packet rates) applies to all numbers until
the harness is removed after characterization (`docs/30` Phase B). The
probes are gated by the build: Kconfig `USBINF_PERF_PROBES` (default y,
while the bench runs) compiles begin/end to no-ops when turned off, and the
`t_*` telemetry keys then simply stop appearing — production images are
built with it off after the final matrix pass.

## 2. Telemetry

`tools/net_stats.py <ip> watch`, per window:

- `t_<probe>_n`, `t_<probe>_p50`, `t_<probe>_p99`, `t_<probe>_max`
  (cycles; p50/p99 at 8-cycle resolution), `t_<probe>_ovf` (samples > 255).
- `net_rx_dma_free`: free RX descriptors (0 = ring full, frames held in the
  MTL FIFO). Sustained 0 during bursts means the RX DMA ring, not the CPU,
  is the limit.

Bench order: docs/21 matrix C→B→A; record `cpu0/1_idle_pct` beside the
windows. Decision rules added to §7 below.

## 3. Applied (no-regret) optimizations

Each is behavior-preserving and host-tested (`tools/host_tests.sh`):

- **Incremental packet timestamp** (`aoip/ts_incr.h`, `flows_tx.c`): the
  media header's (seconds, subsec) used two 64-bit divisions per TX packet
  (`ts/48000`, `ts%48000`); now computed once per (re)anchor and advanced by
  fpp with one conditional carry. ~40-80 cycles off every TX packet.
  Verified: 800k advance steps vs div/mod.
- **239/8 membership bitmap** (`net.c`, `net.h net_group_bit`): `group_wanted`
  scanned 32 atomic slots per unmatched-mcast frame (the flood case); now one
  exact 23-bit hash of the group's low octets into 8 words. Join/leave update
  the bitmap for 239/8 only (other scopes must not use it — PTP groups
  collide with 239 groups sharing octets 2-4, and the filter only consults
  239/8 destinations). Verified bijective over a 239/8 sample.

## 4. Gated backlog (apply only if the windows indict them)

- **Peak metering out of the RX decode loop** (`flows_rx.c:164-198` computes
  per-channel `abs32`+max per sample, ≈ +30 % inner-loop cost). NOT movable to
  the heartbeat ring scan: USB IN clears the frames it reads, so a ring
  snapshot only sees ~1.3 ms of audio. Options: per-packet post-pass (same
  work count), or accept coarse meters. Gate: `t_rx_hook_p99` vs decode-only
  cost.
- **Per-destination header template** (`net.c build_headers`): the checksum
  is linear in total_length + IP ID, so a cached 42-byte header + 3 stores +
  a 2-fold fold replaces the full rebuild per packet (~50-80 cycles). Source
  address staleness needs a refresh check in `ftx_tick` (IP can change
  without re-resolve), which is why it is not applied blind. This is also
  the groundwork for Proposal 2's direct-DMA TX build.
- **`esp_timer_get_time()` per RX packet** (`flows_rx.c:123`, keepalive
  staleness): cheap; gate on its absence from p99.
- **`-O3`/LTO for the three hot files, TCM placement of `s_pkt` + maps,
  decode-loop flat maps, Zbb `rev8` if the core has it**: Proposal 3 items,
  each needs the bench matrix (and `check_iram.py`) to sign off.

## 5. What decides Proposals 2 and 3

| reading | meaning | next step |
|---|---|---|
| `t_rx_input_p99` high, `net_rx_dropped`/0 free desc in bursts | driver malloc+copy tax dominates | Proposal 2 (borrowed-buffer RX) |
| `t_tx_send_p99` ≈ `t_tx_build_p99` | send path dominates | Proposal 2 (DMA TX build) |
| `t_usb_sof/_xfer_p99` > ~2 µs | USB ISR budget at risk | Proposal 3 (IRAM/TCM, placement) |
| `cpu0_idle_pct` low with all p99 small | scheduling/latency issue | Packetizer wake economics, core rebalance |
| all p99 small, `cpu_idle` > 80 % | CPU not the limit | stop optimizing; profile 4/2 viable as is |
