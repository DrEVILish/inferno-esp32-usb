#!/bin/bash
# Re-create the project-local esp_eth override (firmware/components/esp_eth)
# from the IDF in use, then apply our patches. Run after an IDF upgrade, build,
# and check that the patch still applies cleanly (A4: RX DMA ring > 30).
#   tools/esp_eth_override.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. /opt/usb-inferno/tools/idf-env.sh
DST="$ROOT/firmware/components/esp_eth"
SRC="$IDF_PATH/components/esp_eth"
# Validate before destroying anything: a missing source left the tree
# unbuildable (docs/13 §6); a fuzzed patch can land in the wrong place and
# look fine (exit 0). Fuzz 0 + a dry run + a .rej check keep it honest.
if [ ! -d "$SRC" ]; then
    echo "esp_eth_override: $SRC not found (IDF export failed?)" >&2
    exit 1
fi
cd "$ROOT/firmware" && patch --dry-run --fuzz=0 -p1 --no-backup-if-mismatch \
    < "$ROOT/patches/esp_eth-rx64.patch"
rm -rf "$DST"
cp -r "$SRC" "$DST"
rm -rf "$DST/test_apps"
patch -p1 --fuzz=0 --no-backup-if-mismatch < "$ROOT/patches/esp_eth-rx64.patch"
if find "$ROOT/firmware/components" -name '*.rej' | grep -q .; then
    echo "esp_eth_override: patch rejected hunks present" >&2
    exit 1
fi
echo "esp_eth override refreshed from $IDF_PATH"
