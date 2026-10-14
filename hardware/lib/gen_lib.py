#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Generate the project-local KiCad library for usb-inferno rev 1.

Writes
  usb_inferno.kicad_sym                     symbols not in KiCad 9's libraries
  usb_inferno.pretty/*.kicad_mod            footprints not in KiCad 9's libraries

Sources for every pin and dimension are given next to the data:
  [P4-DS]  ESP32-P4 Series Datasheet v0.7, Table 2-1 (pin overview), Fig. 6-1 (QFN104 package)
  [IP-DS]  IC Plus IP101G Preliminary Data Sheet IP101G-DS-R01, IP101GR/GRI pin table, Fig. 25
  [TPS-DS] TI TPS25200 data sheet SLVSCJ0F, Table 4-1 (DRV package)

Run:  python3 hardware/lib/gen_lib.py   (no dependencies)
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = "usb_inferno"

# ---------------------------------------------------------------------------
# ESP32-P4 QFN104 pin map  [P4-DS Table 2-1]; pin 54 = VDD_HP_1 on chip rev v3.x
# (NC on v1.x)  [ESP32-P4 Chip Revision v3.x User Guide v1.1, 2.1.1]
# ---------------------------------------------------------------------------
P4_PINS = {
    1: "GPIO1", 2: "GPIO2", 3: "GPIO3", 4: "GPIO4", 5: "GPIO5", 6: "GPIO6",
    7: "GPIO7", 8: "GPIO8", 9: "VDD_LP", 10: "GPIO9", 11: "GPIO10",
    12: "GPIO11", 13: "GPIO12", 14: "GPIO13", 15: "GPIO14", 16: "GPIO15",
    17: "GPIO16", 18: "GPIO17", 19: "GPIO18", 20: "GPIO19", 21: "VDD_IO_0",
    22: "GPIO20", 23: "GPIO21", 24: "GPIO22", 25: "GPIO23", 26: "VDD_HP_0",
    27: "FLASH_CS", 28: "FLASH_Q", 29: "FLASH_WP", 30: "VDD_FLASHIO",
    31: "FLASH_HOLD", 32: "FLASH_CK", 33: "FLASH_D", 34: "DSI_REXT",
    35: "DSI_DATAP1", 36: "DSI_DATAN1", 37: "DSI_CLKN", 38: "DSI_CLKP",
    39: "DSI_DATAP0", 40: "DSI_DATAN0", 41: "VDD_MIPI_DPHY", 42: "CSI_DATAN0",
    43: "CSI_DATAP0", 44: "CSI_CLKP", 45: "CSI_CLKN", 46: "CSI_DATAN1",
    47: "CSI_DATAP1", 48: "CSI_REXT", 49: "USB_DM", 50: "USB_DP",
    51: "VDD_USBPHY", 52: "GPIO24", 53: "GPIO25", 54: "VDD_HP_1",
    55: "GPIO26", 56: "GPIO27", 57: "GPIO28", 58: "GPIO29", 59: "VDD_PSRAM_0",
    60: "GPIO30", 61: "GPIO31", 62: "VDD_IO_4", 63: "GPIO32", 64: "GPIO33",
    65: "GPIO34", 66: "GPIO35", 67: "VDD_PSRAM_1", 68: "GPIO36", 69: "GPIO37",
    70: "GPIO38", 71: "VDDO_FLASH", 72: "VDDO_PSRAM", 73: "VDDO_3",
    74: "VDDO_4", 75: "VDD_LDO", 76: "VDD_HP_2", 77: "VDD_DCDCC",
    78: "FB_DCDC", 79: "EN_DCDC", 80: "GPIO39", 81: "GPIO40", 82: "GPIO41",
    83: "GPIO42", 84: "GPIO43", 85: "VDD_IO_5", 86: "GPIO44", 87: "GPIO45",
    88: "GPIO46", 89: "GPIO47", 90: "GPIO48", 91: "VDD_HP_3", 92: "GPIO49",
    93: "GPIO50", 94: "GPIO51", 95: "GPIO52", 96: "VDD_IO_6", 97: "GPIO53",
    98: "GPIO54", 99: "XTAL_N", 100: "XTAL_P", 101: "VDD_ANA", 102: "VDD_BAT",
    103: "CHIP_PU", 104: "GPIO0", 105: "GND",
}


def p4_type(name):
    if name == "GND" or name.startswith("VDD_"):
        return "power_in"
    if name.startswith("VDDO_"):
        return "power_out"
    if name in ("FLASH_CS", "FLASH_CK", "EN_DCDC"):
        return "output"
    if name == "CHIP_PU":
        return "input"
    if name.startswith(("GPIO", "FLASH_", "USB_")):
        return "bidirectional"
    return "passive"   # XTAL, FB_DCDC, MIPI


def p4_symbol_pins():
    """Left: power/system. Right: GPIO + dedicated interfaces."""
    left, right = [], []
    for num, name in P4_PINS.items():
        (left if (name.startswith(("VDD", "GND", "XTAL", "CHIP_PU", "FB_", "EN_DCDC"))) else right).append((num, name))
    order_l = ["CHIP_PU", "XTAL_P", "XTAL_N", "EN_DCDC", "FB_DCDC"]
    left.sort(key=lambda p: (0, order_l.index(p[1])) if p[1] in order_l else (2 if p[1] == "GND" else 1, p[1]))

    def rkey(p):
        n = p[1]
        if n.startswith("GPIO"):
            return (0, int(n[4:]))
        if n.startswith("FLASH"):
            return (1, n)
        if n.startswith("USB"):
            return (2, n)
        return (3, n)
    right.sort(key=rkey)
    return [(n, nm, p4_type(nm), "L") for n, nm in left] + [(n, nm, p4_type(nm), "R") for n, nm in right]


# ---------------------------------------------------------------------------
# IP101GRI QFN32  [IP-DS, IP101GR/GRI pin table]
# ---------------------------------------------------------------------------
IP101_PINS = [
    (1, "TXER/FXSD", "input", "L"), (5, "TXEN", "input", "L"), (9, "TXD0", "input", "L"),
    (8, "TXD1", "input", "L"), (7, "TXD2", "input", "L"), (6, "TXD3", "input", "L"),
    (10, "TXCLK/50M_CLKI", "bidirectional", "L"), (14, "RXCLK/50M_CLKO", "output", "L"),
    (19, "RXDV/CRS_DV/FX_HEN", "bidirectional", "L"), (18, "RXD0", "output", "L"),
    (17, "RXD1", "output", "L"), (16, "RXD2", "output", "L"), (15, "RXD3", "output", "L"),
    (21, "RXER/INTR_32", "output", "L"), (4, "COL/RMII", "bidirectional", "L"),
    (20, "CRS/LEDMOD", "bidirectional", "L"), (22, "MDC", "input", "L"),
    (23, "MDIO", "bidirectional", "L"), (32, "RESET_N", "input", "L"),
    (2, "X1", "input", "L"), (3, "X2", "output", "L"),
    (30, "MDI_TP", "bidirectional", "R"), (29, "MDI_TN", "bidirectional", "R"),
    (27, "MDI_RP", "bidirectional", "R"), (26, "MDI_RN", "bidirectional", "R"),
    (11, "LED0/PHY_AD0", "bidirectional", "R"), (12, "LED3/PHY_AD3", "bidirectional", "R"),
    (24, "TEST_ON", "input", "R"), (25, "ISET", "passive", "R"),
    (28, "REGOUT", "power_out", "R"), (13, "VDDIO", "power_in", "R"),
    (31, "AVDD33", "power_in", "R"), (33, "GND", "power_in", "R"),
]

# ---------------------------------------------------------------------------
# TPS25200 WSON-6 (DRV)  [TPS-DS Table 4-1]; PowerPAD -> footprint pad 7
# ---------------------------------------------------------------------------
TPS25200_PINS = [
    (6, "IN", "power_in", "L"), (4, "EN", "input", "L"), (2, "ILIM", "passive", "L"),
    (1, "OUT", "power_out", "R"), (3, "~{FAULT}", "open_collector", "R"),
    (5, "GND", "power_in", "R"), (7, "PAD", "passive", "R"),
]


def fmt(v):
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def symbol(name, ref, value, footprint, datasheet, descr, pins, width=25.4, extra_props=()):
    left = [p for p in pins if p[3] == "L"]
    right = [p for p in pins if p[3] == "R"]
    n = max(len(left), len(right))
    pitch = 2.54
    top = (n - 1) * pitch / 2
    top = round(top / 1.27) * 1.27
    h2 = top + pitch
    w2 = width / 2
    L = 5.08
    out = []
    out.append('\t(symbol "%s"' % name)
    out.append('\t\t(pin_names (offset 1.016))')
    out.append('\t\t(exclude_from_sim no) (in_bom yes) (on_board yes)')
    props = [("Reference", ref, 0, h2 + 1.27), ("Value", value, 0, -h2 - 1.27),
             ("Footprint", footprint, 0, -h2 - 3.81), ("Datasheet", datasheet, 0, -h2 - 6.35),
             ("Description", descr, 0, -h2 - 8.89)] + [(k, v, 0, -h2 - 11.43 - 2.54 * i) for i, (k, v) in enumerate(extra_props)]
    for k, v, x, y in props:
        hide = "" if k in ("Reference", "Value") else " (hide yes)"
        out.append('\t\t(property "%s" "%s" (at %s %s 0) (effects (font (size 1.27 1.27))%s))' % (k, v, fmt(x), fmt(y), hide))
    out.append('\t\t(symbol "%s_0_1"' % name)
    out.append('\t\t\t(rectangle (start %s %s) (end %s %s) (stroke (width 0.254) (type default)) (fill (type background)))'
               % (fmt(-w2), fmt(h2), fmt(w2), fmt(-h2)))
    out.append('\t\t)')
    out.append('\t\t(symbol "%s_1_1"' % name)
    for side, plist in (("L", left), ("R", right)):
        for i, (num, pname, ptype, _) in enumerate(plist):
            y = top - i * pitch
            if side == "L":
                x, ang = -w2 - L, 0
            else:
                x, ang = w2 + L, 180
            out.append('\t\t\t(pin %s line (at %s %s %d) (length %s) (name "%s" (effects (font (size 1.27 1.27)))) (number "%s" (effects (font (size 1.27 1.27)))))'
                       % (ptype, fmt(x), fmt(y), ang, fmt(L), pname, num))
    out.append('\t\t)')
    out.append('\t\t(embedded_fonts no)')
    out.append('\t)')
    return "\n".join(out)


def write_symbols():
    syms = [
        symbol("ESP32-P4NRW32X", "U", "ESP32-P4NRW32X",
               "usb_inferno:Espressif_QFN-104-1EP_10x10mm_P0.35mm_EP7.5x7.5mm",
               "https://documentation.espressif.com/esp32-p4_datasheet_en.pdf",
               "ESP32-P4 chip revision v3.x, 32 MB in-package PSRAM, QFN104 10x10 mm. Pin 54 = VDD_HP_1 (v3.x).",
               p4_symbol_pins(), width=30.48),
        symbol("IP101GRI", "U", "IP101GRI",
               "Package_DFN_QFN:QFN-32-1EP_4x4mm_P0.4mm_EP2.65x2.65mm_ThermalVias",
               "https://www.icplus.com.tw/", "IC Plus 10/100 Ethernet PHY, MII/RMII, QFN32 4x4 mm, -40..85 C",
               IP101_PINS, width=27.94),
        symbol("TPS25200DRV", "U", "TPS25200DRV",
               "Package_SON:WSON-6-1EP_2x2mm_P0.65mm_EP1x1.6mm",
               "https://www.ti.com/lit/ds/symlink/tps25200.pdf",
               "5 V eFuse, 20 V tolerant input, 5.4 V OVC, adjustable current limit, soft start",
               TPS25200_PINS, width=12.7),
    ]
    txt = '(kicad_symbol_lib\n\t(version 20241209)\n\t(generator "usb_inferno_gen_lib")\n\t(generator_version "1.0")\n' + "\n".join(syms) + "\n)\n"
    with open(os.path.join(HERE, LIB + ".kicad_sym"), "w") as f:
        f.write(txt)


# ---------------------------------------------------------------------------
# QFN104 footprint  [P4-DS Fig. 6-1]
#   body 10x10 BSC, e 0.35 BSC, b 0.13/0.18/0.23, L 0.3/0.4/0.5, corner leads L1 0.35 REF,
#   EP 7.4/7.5/7.6, K (lead tip to EP edge) 0.85 REF, 26 leads per side, pin 1 top-left,
#   numbering anti-clockwise in top view.
# Land pattern (own derivation, IPC-7351-like, verify against Espressif's KiCad/Altium
# library before release - UNVERIFIED): pad 0.20 x 0.70 mm from 4.55 to 5.25 mm off centre
# (0.25 mm toe, 0.05 mm heel), corner pads shortened to 0.60 mm (start 4.65 mm) so adjacent
# corner pads keep >= 0.2 mm; EP 7.5 x 7.5 mm, 4x4 paste windows (~56 %), 3x3 0.25 mm vias between them.
# ---------------------------------------------------------------------------
def qfn104_footprint():
    name = "Espressif_QFN-104-1EP_10x10mm_P0.35mm_EP7.5x7.5mm"
    e, n = 0.35, 26
    pw = 0.20
    first = -(n - 1) * e / 2
    lines = []
    A = lines.append
    A('(footprint "%s"' % name)
    A('\t(version 20241229)\n\t(generator "usb_inferno_gen_lib")\n\t(generator_version "1.0")\n\t(layer "F.Cu")')
    A('\t(descr "Espressif ESP32-P4 QFN104 10x10 mm, 0.35 mm pitch, EP 7.5 mm (ESP32-P4 datasheet v0.7 Fig. 6-1). Land pattern derived, verify.")')
    A('\t(tags "QFN NoLead ESP32-P4")')
    A('\t(property "Reference" "REF**" (at 0 -6.6 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))')
    A('\t(property "Value" "%s" (at 0 6.6 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))' % name)
    A('\t(attr smd)')
    # fab outline + pin-1 chamfer
    A('\t(fp_poly (pts (xy -4 -5) (xy 5 -5) (xy 5 5) (xy -5 5) (xy -5 -4)) (stroke (width 0.1) (type solid)) (fill no) (layer "F.Fab"))')
    # silk corner marks
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = 5.11 * sx, 5.11 * sy
            A('\t(fp_line (start %s %s) (end %s %s) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))' % (fmt(x), fmt(y), fmt(x - 0.45 * sx), fmt(y)))
            if not (sx == -1 and sy == -1):
                A('\t(fp_line (start %s %s) (end %s %s) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))' % (fmt(x), fmt(y), fmt(x), fmt(y - 0.45 * sy)))
    A('\t(fp_circle (center -5.6 -5.0) (end -5.45 -5.0) (stroke (width 0.3) (type solid)) (fill yes) (layer "F.SilkS"))')
    A('\t(fp_rect (start -5.5 -5.5) (end 5.5 5.5) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))')
    A('\t(fp_text user "${REFERENCE}" (at 0 0 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))')

    def pad(num, x, y, w, h):
        A('\t(pad "%d" smd roundrect (at %s %s) (size %s %s) (layers "F.Cu" "F.Mask" "F.Paste") (roundrect_rratio 0.25))'
          % (num, fmt(x), fmt(y), fmt(w), fmt(h)))

    corner = {1, 26, 27, 52, 53, 78, 79, 104}
    for i in range(n):
        pos = first + i * e
        for side in range(4):
            num = side * n + i + 1
            ln, c = (0.60, 4.95) if num in corner else (0.70, 4.90)
            if side == 0:      # left, top -> bottom
                pad(num, -c, pos, ln, pw)
            elif side == 1:    # bottom, left -> right
                pad(num, pos, c, pw, ln)
            elif side == 2:    # right, bottom -> top
                pad(num, c, -pos, ln, pw)
            else:              # top, right -> left
                pad(num, -pos, -c, pw, ln)
    # exposed pad 105 (copper + mask, no paste; paste windows separately)
    A('\t(pad "105" smd rect (at 0 0) (size 7.5 7.5) (layers "F.Cu" "F.Mask") (zone_connect 2))')
    win, gap = 1.4, 1.9
    for ix in range(4):
        for iy in range(4):
            x = (ix - 1.5) * gap
            y = (iy - 1.5) * gap
            A('\t(pad "" smd roundrect (at %s %s) (size %s %s) (layers "F.Paste") (roundrect_rratio 0.15))' % (fmt(x), fmt(y), fmt(win), fmt(win)))
    # 3x3 thermal vias in the gaps between the paste windows (0, +-1.9 mm)
    for ix in (-1, 0, 1):
        for iy in (-1, 0, 1):
            A('\t(pad "105" thru_hole circle (at %s %s) (size 0.45 0.45) (drill 0.25) (layers "*.Cu") (zone_connect 2))' % (fmt(ix * 1.9), fmt(iy * 1.9)))
    A('\t(embedded_fonts no)')
    A(')')
    d = os.path.join(HERE, LIB + ".pretty")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".kicad_mod"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    write_symbols()
    qfn104_footprint()
    print("wrote", LIB + ".kicad_sym and", LIB + ".pretty/")
