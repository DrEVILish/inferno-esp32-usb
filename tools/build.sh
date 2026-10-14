#!/bin/bash
# Build the firmware in the given tree (default: this repo/worktree).
# Builds are serialized machine-wide (flock) and run on 2 cores (taskset 12,13) at nice 10 so that
# parallel agents never overload the container.
# After a successful build, tools/check_iram.py verifies that everything the
# USB interrupt can reach is in IRAM (it must survive flash writes); a failure
# fails the build.
set -e
TREE="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
. /opt/usb-inferno/tools/idf-env.sh
cd "$TREE/firmware"
flock /tmp/usb-inferno-build.lock nice -n 10 taskset -c 12,13 \
    idf.py build "${@:2}"
if ! python3 "$TREE/tools/check_iram.py" build/usb_inferno.elf; then
    echo "*** BUILD CHECK FAILED: flash-resident code/data on the USB ISR path (see above) ***" >&2
    exit 1
fi
