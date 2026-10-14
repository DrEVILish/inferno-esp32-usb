#!/bin/bash
# Build and run the host simulation of the USB clock/position engine
# (firmware/main/usb/uac2_engine.c). Light: plain gcc, one at a time.
#
# Runs every latency profile (10/6/4/2: USB OUT lead and IN margin differ)
# in the production and the CONFIG_USBINF_TEST_USB_LOOPBACK variant, at 32
# channels, plus a 2-channel build of the default profile.
#   tools/usb_engine_sim.sh            all of the above
#   tools/usb_engine_sim.sh 6          one profile (production + loopback)
#   LEAD=16 tools/usb_engine_sim.sh 6  override the OUT lead (sweeps)
set -e
rc=0
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/usb_engine_sim.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
CF="-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DUAC2_HOST_SIM -I$ROOT/firmware/main"
[ -n "$LEAD" ] && CF="$CF -DUI_SIM_OUT_LEAD=$LEAD"
SRC="$ROOT/tools/usb_engine_sim.c $ROOT/firmware/main/usb/uac2_engine.c"
run() {     # name, extra cflags
    echo "### $1"
    nice gcc $CF $2 -o "$OUT/sim" $SRC -lm
    nice "$OUT/sim" || rc=1
    echo
}
PROFILES="${1:-10 6 4 2}"
for P in $PROFILES; do
    run "profile $P, production" "-DUI_LAT_PROFILE=$P"
    run "profile $P, USB loopback" "-DUI_LAT_PROFILE=$P -DCONFIG_USBINF_TEST_USB_LOOPBACK=1"
done
[ -z "$1" ] && run "profile 10, 2 channels, USB loopback" "-DUI_LAT_PROFILE=10 -DUI_CHANNELS=2u -DCONFIG_USBINF_TEST_USB_LOOPBACK=1"
echo "### summary: $([ $rc = 0 ] && echo ALL PASSED || echo FAILURES)"
exit $rc
