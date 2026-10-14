#!/bin/bash
# Build latency/channel test-matrix images (docs/21-latency-matrix.md) into
# firmware/build-matrix/<name>/, name = LAT<profile>_CH<channels>[_NETLOOP|_USBLOOP].
#
#   tools/build-matrix.sh                     all 60 (4 profiles x 5 channel counts x 3 modes)
#   tools/build-matrix.sh LAT6_CH32 LAT2_CH2_USBLOOP ...   just these
#   tools/build-matrix.sh --list              print the names
#
# Every build goes through the machine-wide build lock, 2 cores (taskset
# 12,13), nice 10, one at a time, and is followed by the IRAM check
# (tools/check_iram.py). Flash one:
#   . tools/idf-env.sh && cd firmware && idf.py -B build-matrix/<name> -p /dev/ttyACM0 flash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
all_names() {
    for P in 10 6 4 2; do for C in 32 16 8 4 2; do for M in "" _NETLOOP _USBLOOP; do
        echo "LAT${P}_CH${C}${M}"
    done; done; done
}
if [ "$1" = "--list" ]; then all_names; exit 0; fi
NAMES="${*:-$(all_names)}"

. /opt/usb-inferno/tools/idf-env.sh
cd "$ROOT/firmware"
fail=0
for N in $NAMES; do
    if ! [[ "$N" =~ ^LAT(10|6|4|2)_CH(32|16|8|4|2)(_NETLOOP|_USBLOOP)?$ ]]; then
        echo "bad name: $N (LAT<10|6|4|2>_CH<32|16|8|4|2>[_NETLOOP|_USBLOOP])" >&2; exit 2
    fi
    P="${BASH_REMATCH[1]}"; C="${BASH_REMATCH[2]}"; M="${BASH_REMATCH[3]}"
    B="build-matrix/$N"
    mkdir -p "$B"
    {
        echo "CONFIG_USBINF_LAT_$P=y"
        echo "CONFIG_USBINF_CH_$C=y"
        [ "$M" = _NETLOOP ] && echo "CONFIG_USBINF_TEST_NET_LOOPBACK=y"
        [ "$M" = _USBLOOP ] && echo "CONFIG_USBINF_TEST_USB_LOOPBACK=y"
        true
    } > "$B/matrix.defaults"
    echo "=== $N"
    if flock /tmp/usb-inferno-build.lock nice -n 10 taskset -c 12,13 \
            idf.py -B "$B" -DSDKCONFIG="$B/sdkconfig" \
            -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$B/matrix.defaults" build > "$B/build.log" 2>&1 &&
       python3 "$ROOT/tools/check_iram.py" "$B/usb_inferno.elf" >> "$B/build.log" 2>&1; then
        echo "    ok ($(tail -1 "$B/build.log"))"
    else
        echo "    FAILED, see firmware/$B/build.log" >&2
        tail -20 "$B/build.log" >&2
        fail=1
    fi
done
exit $fail
