#!/bin/bash
# Re-create the project-local esp_eth override (firmware/components/esp_eth)
# from the IDF in use, then apply our patches. Run after an IDF upgrade, build,
# and check that the patch still applies cleanly (A4: RX DMA ring > 30).
#   tools/esp_eth_override.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. /opt/usb-inferno/tools/idf-env.sh
DST="$ROOT/firmware/components/esp_eth"
rm -rf "$DST"
cp -r "$IDF_PATH/components/esp_eth" "$DST"
rm -rf "$DST/test_apps"
cd "$ROOT/firmware"
patch -p1 --no-backup-if-mismatch < "$ROOT/patches/esp_eth-rx64.patch"
echo "esp_eth override refreshed from $IDF_PATH"
