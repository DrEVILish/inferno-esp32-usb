#!/bin/sh
# Regenerate everything under hardware/ (run from anywhere). Heavy steps are niced.
set -e
HW=$(cd "$(dirname "$0")" && pwd)
VENV=${VENV:-/opt/usb-inferno/work/venv-hw}
cd "$HW"
python3 lib/gen_lib.py                                    # custom symbols/footprints
nice -n 10 "$VENV/bin/python" usb_inferno.py 2>&1 \
  | grep -v "Missing tag\|Random tag\|environment variable is missing\|fp-lib-table" | tee erc.log
nice -n 10 python3 tools/make_pcb.py                      # system python3 (KiCad 9 pcbnew)
nice -n 10 kicad-cli pcb drc -o kicad/drc-unrouted.rpt kicad/usb-inferno.kicad_pcb | tail -3
