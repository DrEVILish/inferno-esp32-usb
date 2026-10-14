#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Query the usb-inferno UDP telemetry (port 7779).

Any datagram to the port returns one "key=value" snapshot.

  net_stats.py HOST                       one snapshot, all keys
  net_stats.py HOST get KEY [KEY ...]     selected keys
  net_stats.py HOST watch [SECONDS] [--interval S] [--csv FILE]
                                          follow the PTP servo; prints one line
                                          per poll and a summary at the end
                                          (lock time, offset statistics while
                                          locked, steps / lock losses seen)

Exit status of `watch`: 0 if locked at the end, 1 otherwise.
"""
import argparse
import socket
import statistics
import sys
import time

PORT = 7779


def snapshot(host, timeout=0.5, tries=3):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        for _ in range(tries):
            s.sendto(b"?", (host, PORT))
            try:
                data, _ = s.recvfrom(8192)
            except socket.timeout:
                continue
            out = {}
            for line in data.decode(errors="replace").splitlines():
                k, sep, v = line.partition("=")
                if sep:
                    out[k.strip()] = v.strip()
            return out
        return None
    finally:
        s.close()


def as_int(d, k, default=0):
    try:
        return int(d.get(k, default))
    except (TypeError, ValueError):
        return default


WATCH_KEYS = ("ptp_locked", "ptp_stage", "ptp_offset_ns", "ptp_freq_ppb",
              "ptp_path_delay_ns", "ptp_epoch", "ptp_steps", "ptp_lock_losses",
              "ptp_outliers", "ptp_syncs", "ptp_delay_resps", "ptp_leader")


def watch(host, seconds, interval, csv_path):
    t0 = time.monotonic()
    first = None
    lock_at = None
    offsets = []
    misses = 0
    csv = open(csv_path, "w") if csv_path else None
    if csv:
        csv.write("t_s," + ",".join(WATCH_KEYS) + "\n")
    print(f"{'t/s':>7} {'lock':>4} {'stg':>3} {'offset/ns':>10} {'freq/ppb':>9} "
          f"{'delay/ns':>9} {'epoch':>5} {'steps':>5} {'losses':>6} {'outl':>5} {'syncs':>6}  leader")
    last = None
    while seconds <= 0 or time.monotonic() - t0 < seconds:
        try:
            d = snapshot(host)
        except KeyboardInterrupt:
            break
        t = time.monotonic() - t0
        if d is None:
            misses += 1
            print(f"{t:7.1f}  (no reply)")
            time.sleep(interval)
            continue
        if first is None:
            first = d
        last = d
        locked = as_int(d, "ptp_locked")
        if locked and lock_at is None:
            lock_at = t
        if locked:
            offsets.append(as_int(d, "ptp_offset_ns"))
        print(f"{t:7.1f} {locked:>4} {as_int(d, 'ptp_stage'):>3} {as_int(d, 'ptp_offset_ns'):>10} "
              f"{as_int(d, 'ptp_freq_ppb'):>9} {as_int(d, 'ptp_path_delay_ns'):>9} "
              f"{as_int(d, 'ptp_epoch'):>5} {as_int(d, 'ptp_steps'):>5} {as_int(d, 'ptp_lock_losses'):>6} "
              f"{as_int(d, 'ptp_outliers'):>5} {as_int(d, 'ptp_syncs'):>6}  {d.get('ptp_leader', '')}")
        if csv:
            csv.write(f"{t:.2f}," + ",".join(str(d.get(k, "")) for k in WATCH_KEYS) + "\n")
            csv.flush()
        try:
            time.sleep(interval)
        except KeyboardInterrupt:
            break
    if csv:
        csv.close()

    print("\nsummary")
    if last is None:
        print("  no replies")
        return 1
    up0 = as_int(first, "uptime_ms") / 1000.0
    print(f"  device uptime at start {up0:.1f} s, replies missed {misses}")
    if lock_at is None:
        print("  never locked")
    else:
        print(f"  first seen locked {lock_at:.1f} s into the watch "
              f"(device uptime ~{up0 + lock_at:.1f} s)")
    if offsets:
        a = [abs(o) for o in offsets]
        a.sort()
        p95 = a[min(len(a) - 1, int(len(a) * 0.95))]
        print(f"  locked offset: n={len(offsets)} mean {statistics.mean(offsets):.0f} ns, "
              f"stdev {statistics.pstdev(offsets):.0f} ns, |p95| {p95} ns, |max| {a[-1]} ns")
    for k in ("ptp_steps", "ptp_lock_losses", "ptp_outliers", "ptp_phase_shifts",
              "ptp_leader_changes", "ptp_no_hw_rx_ts", "ptp_no_hw_tx_ts", "ptp_queue_drops",
              "net_rx_dropped", "net_tx_raw_fail"):
        if k in last:
            print(f"  {k}: {as_int(first, k)} -> {as_int(last, k)}")
    return 0 if as_int(last, "ptp_locked") else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("cmd", nargs="?", default="dump", choices=("dump", "get", "watch"))
    ap.add_argument("args", nargs="*")
    ap.add_argument("--interval", type=float, default=1.0, help="watch poll interval, s")
    ap.add_argument("--csv", help="watch: also write samples to this CSV file")
    o = ap.parse_args()

    if o.cmd == "watch":
        secs = float(o.args[0]) if o.args else 0.0
        return watch(o.host, secs, o.interval, o.csv)
    d = snapshot(o.host)
    if d is None:
        print(f"no reply from {o.host}:{PORT}", file=sys.stderr)
        return 2
    if o.cmd == "get":
        for k in o.args:
            print(f"{k}={d.get(k, '')}")
    else:
        for k, v in d.items():
            print(f"{k}={v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
