#!/bin/bash
# Host test for the fork-comparable AoIP replies (no IDF, no hardware).
# Compiles the REAL firmware/main/aoip/{arc,wire,info_mcast}.c against stub
# headers (tools/host_stub) and fake state (tools/aoip_host_stubs.c), then
# checks every reply byte against vectors from DrEVILish/inferno.
#   tools/aoip_host_test.sh            build + run, asserts
#   DUMP=1 tools/aoip_host_test.sh     print reply hex (for check_vectors.py)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/aoip_host_test.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
gcc -O1 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DUAC2_HOST_SIM \
    -I"$ROOT/firmware/main" -I"$ROOT/tools/host_stub" \
    "$ROOT/tools/arc_info_host_test.c" \
    "$ROOT/tools/aoip_host_stubs.c" \
    "$ROOT/firmware/main/aoip/arc.c" \
    "$ROOT/firmware/main/aoip/wire.c" \
    "$ROOT/firmware/main/aoip/info_mcast.c" \
    -o "$OUT/aoip_host_test" -lm
"$OUT/aoip_host_test"
