#!/bin/bash
# Run every host test (no IDF, no hardware):
#   tools/host_tests.sh
# Tests: fpp selection, perf histogram math, timestamp split + group bitmap,
# AoIP fork-comparable replies (byte-exact vectors).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/host_tests.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
CF="-O1 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DUAC2_HOST_SIM -I$ROOT/firmware/main -I$ROOT/tools/host_stub"

echo "== fpp_select_test"
gcc $CF "$ROOT/tools/fpp_select_test.c" -o "$OUT/fpp" && "$OUT/fpp"

echo "== mdns_txt_test (TXT values range-checked before narrowing)"
gcc $CF "$ROOT/tools/mdns_txt_test.c" "$ROOT/firmware/main/aoip/mdns_txt.c" -o "$OUT/mdns_txt" && "$OUT/mdns_txt"

echo "== checksum_test"
gcc -O1 -Wall -Wextra "$ROOT/tools/checksum_test.c" -o "$OUT/cksum" && "$OUT/cksum"

echo "== perf_host_test"
gcc $CF "$ROOT/tools/perf_host_test.c" "$ROOT/firmware/main/sys/perf.c" -o "$OUT/perf" && "$OUT/perf"

echo "== ts_bitmap_test"
gcc $CF "$ROOT/tools/ts_bitmap_test.c" -o "$OUT/ts" && "$OUT/ts"

echo "== aoip_host_test (arc/info replies vs fork vectors)"
"$ROOT/tools/aoip_host_test.sh"
