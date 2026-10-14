#!/bin/bash
# Build a bench test variant into firmware/build-<VARIANT>/ :
#   tools/build-variant.sh USB_LOOPBACK | NET_LOOPBACK | TONE
# Flash: . tools/idf-env.sh && cd firmware && idf.py -B build-<VARIANT> -p /dev/ttyACM0 flash
# The IRAM check (tools/check_iram.py) runs after the build, as in build.sh.
# Latency profiles / channel counts: tools/build-matrix.sh.
set -e
V="${1:?variant: USB_LOOPBACK | NET_LOOPBACK | TONE}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. /opt/usb-inferno/tools/idf-env.sh
cd "$ROOT/firmware"
mkdir -p "build-$V"
echo "CONFIG_USBINF_TEST_$V=y" > "build-$V/variant.defaults"
flock /tmp/usb-inferno-build.lock nice -n 10 taskset -c 12,13 \
    idf.py -B "build-$V" -DSDKCONFIG="build-$V/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;build-$V/variant.defaults" build
if ! python3 "$ROOT/tools/check_iram.py" "build-$V/usb_inferno.elf"; then
    echo "*** BUILD CHECK FAILED: flash-resident code/data on the USB ISR path (see above) ***" >&2
    exit 1
fi
