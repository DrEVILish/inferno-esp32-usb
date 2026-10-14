#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Build hardware/kicad/usb-inferno.kicad_pcb from the SKiDL netlist.

Uses KiCad 9's pcbnew Python module (system python3, not the venv):
    python3 hardware/tools/make_pcb.py

What it does
  * loads every footprint named in usb-inferno.net and assigns pad nets
  * 4-layer board, 60 x 40 mm outline, JLC04161H-3313 stackup written into setup
  * places connectors on the left (short) edge and the main ICs per the placement
    plan in docs/40-board-rev1.md section 11; passives are staged in labelled groups
    to the right of the outline for the layout pass
  * GND zone on In1.Cu (unfilled until the layout pass; fill with B in pcbnew)
  * writes usb-inferno.kicad_pro (net classes, patterns, JLC minimums) and
    usb-inferno.kicad_dru (diff-pair / length rules)
It never routes.
"""
import json
import os
import re
import sys

import pcbnew

HW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KDIR = os.path.join(HW, "kicad")
NET = os.path.join(KDIR, "usb-inferno.net")
PCB = os.path.join(KDIR, "usb-inferno.kicad_pcb")
PRO = os.path.join(KDIR, "usb-inferno.kicad_pro")
DRU = os.path.join(KDIR, "usb-inferno.kicad_dru")
SYS_FP = "/usr/share/kicad/footprints"
LOCAL_FP = {"usb_inferno": os.path.join(HW, "lib", "usb_inferno.pretty")}

BOARD_W, BOARD_H = 60.0, 40.0
OX, OY = 100.0, 100.0          # board origin in KiCad coordinates (mm)


# ---------------------------------------------------------------------------
# tiny s-expression reader for the KiCad netlist
# ---------------------------------------------------------------------------
def sexp(text):
    tok = re.findall(r'\(|\)|"(?:\\.|[^"\\])*"|[^\s()]+', text)
    stack, cur = [], []
    for t in tok:
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done = cur
            cur = stack.pop()
            cur.append(done)
        else:
            cur.append(t[1:-1] if t.startswith('"') else t)
    return cur[0]


def find(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def val(node, key):
    f = find(node, key)
    return f[0][1] if f and len(f[0]) > 1 else None


def read_netlist():
    root = sexp(open(NET).read())
    comps = []
    for c in find(find(root, "components")[0], "comp"):
        fields = {}
        for fl in find(c, "fields"):
            for f in find(fl, "field"):
                fields[val(f, "name")] = f[2] if len(f) > 2 else ""
        comps.append(dict(ref=val(c, "ref"), value=val(c, "value"), footprint=val(c, "footprint"), fields=fields))
    nets = {}
    for n in find(find(root, "nets")[0], "net"):
        name = val(n, "name")
        for nd in find(n, "node"):
            nets.setdefault((val(nd, "ref"), val(nd, "pin")), name)
    return comps, nets


# ---------------------------------------------------------------------------
# placement plan (board-relative mm, rotation deg). See docs/40 section 11.
# ---------------------------------------------------------------------------
PLACE = {
    # connectors on the left short edge: USB-C top, RJ45 bottom (aligned to edge below)
    "J1": (None, 8.5, 270),
    "J2": (None, 24.5, 270),
    "D3": (1.6, 17.5, 0),          # status LED on the connector panel edge
    # ESP32-P4: rotated 180 so RMII (orig. right/top-right) and USB HS (orig. bottom-right
    # corner) face the connectors, flash (orig. bottom-left) faces top-right, XTAL bottom-right
    "U1": (38.0, 20.0, 180),
    "U6": (24.5, 24.5, 0),         # IP101GRI between RJ45 and P4
    "X1": (31.0, 29.5, 0),         # 50 MHz osc: equidistant P4 GPIO50 / PHY pin 10
    "Y2": (25.5, 30.8, 0),         # DNP 25 MHz PHY crystal (clock option B), next to IP101 X1/X2
    "Y1": (45.5, 30.0, 0),         # 40 MHz crystal >= 4.5 mm from XTAL pins (HDG 1.4.2)
    "U5": (49.0, 9.0, 90),         # flash near FLASH_* (top-right of rotated P4)
    "U4": (33.5, 34.5, 0),         # VDD_HP buck near EN/FB_DCDC corner
    "L2": (38.0, 35.5, 0),
    "U2": (13.0, 3.6, 0),          # eFuse right behind USB-C VBUS
    "D2": (14.8, 9.0, 90),         # VBUS TVS
    "D1": (10.5, 9.8, 90),         # HS ESD next to D+/D-
    "U3": (18.5, 4.0, 0),          # 3V3 buck
    "L1": (23.3, 4.5, 0),
    "J3": (55.0, 20.0, 90),        # TC2030 on the far end
    "H1": (30.5, 2.6, 0), "H2": (57.4, 2.6, 0), "H3": (57.4, 37.4, 0), "H4": (25.0, 37.4, 0),
}
TP_ROW = (44.5, 34.6)              # test pads row (bottom-right)
STAGE_X = BOARD_W + 6.0            # staging area for passives, right of the outline


def mm(x, y):
    return pcbnew.VECTOR2I_MM(OX + x, OY + y)


def load_fp(fpid):
    lib, name = fpid.split(":")
    path = LOCAL_FP.get(lib, os.path.join(SYS_FP, lib + ".pretty"))
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        sys.exit("footprint not found: " + fpid)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def anchor_of(ref, comp_nets, majors):
    """Pick the IC/connector a passive belongs to: first major sharing a signal net."""
    power = {"GND", "+3V3", "VDD_HP", "VBUS_5V", "VBUS_C", "PHY_3V3", "CHASSIS"}
    mine = [n for n in comp_nets.get(ref, []) if n not in power]
    for m in majors:
        if set(mine) & set(comp_nets.get(m, [])):
            return m
    # decoupling on a shared rail
    for n in comp_nets.get(ref, []):
        for m in majors:
            if n in comp_nets.get(m, []) and n != "GND":
                return m
    return "misc"


def main():
    comps, pinnet = read_netlist()
    board = pcbnew.BOARD()
    board.SetCopperLayerCount(4)
    board.SetLayerName(pcbnew.In1_Cu, "In1.Cu")
    board.SetLayerName(pcbnew.In2_Cu, "In2.Cu")
    ds = board.GetDesignSettings()
    ds.SetBoardThickness(pcbnew.FromMM(1.6))

    netinfo = {}
    for name in sorted(set(pinnet.values())):
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        netinfo[name] = ni

    comp_nets = {}
    for (ref, pin), n in pinnet.items():
        comp_nets.setdefault(ref, []).append(n)

    fps = {}
    for c in comps:
        fp = load_fp(c["footprint"])
        fp.SetReference(c["ref"])
        fp.SetValue(c["value"])
        for k in ("LCSC", "MPN"):
            if c["fields"].get(k):
                fp.SetField(k, c["fields"][k])
                fp.GetFieldByName(k).SetVisible(False)
        if c["fields"].get("DNP"):
            fp.SetDNP(True)
        if c["fields"].get("exclude_from_bom"):
            fp.SetExcludedFromBOM(True)
        for pad in fp.Pads():
            n = pinnet.get((c["ref"], pad.GetNumber()))
            if n:
                pad.SetNet(netinfo[n])
        board.Add(fp)
        fps[c["ref"]] = fp

    # --- place majors
    for ref, (x, y, rot) in PLACE.items():
        fp = fps.get(ref)
        if fp is None:
            continue
        fp.SetOrientationDegrees(rot)
        fp.SetPosition(mm(x if x is not None else 0, y))
        if x is None:   # align connector front (courtyard) to the left edge, 0.6 mm overhang
            bb = fp.GetCourtyard(pcbnew.F_CrtYd).BBox() if hasattr(fp, "GetCourtyard") else fp.GetBoundingBox(False)
            dx = pcbnew.FromMM(OX - 0.6) - bb.GetX()
            fp.Move(pcbnew.VECTOR2I(dx, 0))
    tps = sorted([r for r in fps if r.startswith("TP")], key=lambda r: int(r[2:]))
    for i, r in enumerate(tps):
        fps[r].SetPosition(mm(TP_ROW[0] + (i % 4) * 2.5, TP_ROW[1] + (i // 4) * 2.5))

    # --- stage passives by anchor, in columns to the right of the board
    majors = ["U1", "U6", "U2", "U3", "U4", "U5", "X1", "Y1", "J1", "J2"]
    groups = {}
    for ref in fps:
        if ref in PLACE or ref.startswith("TP"):
            continue
        groups.setdefault(anchor_of(ref, comp_nets, majors), []).append(ref)
    col = 0
    for anchor in majors + ["misc"]:
        refs = sorted(groups.get(anchor, []), key=lambda r: (r[0], int(re.sub(r"\D", "", r))))
        if not refs:
            continue
        x0 = STAGE_X + col * 12.0
        t = pcbnew.PCB_TEXT(board)
        t.SetText("for " + anchor)
        t.SetLayer(pcbnew.Cmts_User)
        t.SetPosition(mm(x0 + 2, -3))
        t.SetTextSize(pcbnew.VECTOR2I_MM(1, 1))
        board.Add(t)
        for i, r in enumerate(refs):
            fps[r].SetPosition(mm(x0 + (i % 3) * 3.6, (i // 3) * 3.0))
        col += 1

    # --- outline
    rect = pcbnew.PCB_SHAPE(board)
    rect.SetShape(pcbnew.SHAPE_T_RECT)
    rect.SetLayer(pcbnew.Edge_Cuts)
    rect.SetStart(mm(0, 0))
    rect.SetEnd(mm(BOARD_W, BOARD_H))
    rect.SetWidth(pcbnew.FromMM(0.1))
    board.Add(rect)

    # --- GND plane on In1 (solid reference for all impedance-controlled top-layer routing)
    for layer, netname, prio in ((pcbnew.In1_Cu, "GND", 0), (pcbnew.B_Cu, "GND", 0)):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(netinfo[netname])
        z.SetAssignedPriority(prio)
        z.SetIsRuleArea(False)
        ol = z.Outline()
        ol.NewOutline()
        for (x, y) in ((0.3, 0.3), (BOARD_W - 0.3, 0.3), (BOARD_W - 0.3, BOARD_H - 0.3), (0.3, BOARD_H - 0.3)):
            ol.Append(pcbnew.FromMM(OX + x), pcbnew.FromMM(OY + y))
        z.SetLocalClearance(pcbnew.FromMM(0.2))
        z.SetMinThickness(pcbnew.FromMM(0.15))
        z.SetThermalReliefGap(pcbnew.FromMM(0.2))
        z.SetThermalReliefSpokeWidth(pcbnew.FromMM(0.25))
        board.Add(z)
    # +3V3 pour on In2 (power layer); VDD_HP / VBUS islands to be added in layout
    z = pcbnew.ZONE(board)
    z.SetLayer(pcbnew.In2_Cu)
    z.SetNet(netinfo["+3V3"])
    ol = z.Outline()
    ol.NewOutline()
    for (x, y) in ((0.5, 0.5), (BOARD_W - 0.5, 0.5), (BOARD_W - 0.5, BOARD_H - 0.5), (0.5, BOARD_H - 0.5)):
        ol.Append(pcbnew.FromMM(OX + x), pcbnew.FromMM(OY + y))
    z.SetLocalClearance(pcbnew.FromMM(0.25))
    board.Add(z)

    # --- comments: keep-outs and reserved areas
    notes = [
        (2, -6, "usb-inferno rev 1 - PLACEMENT ONLY, NOT ROUTED. Passives staged right of outline (Cmts.User labels)."),
        (2, -4.5, "Stackup JLC04161H-3313 1.6 mm: L1 sig | L2 GND (solid) | L3 +3V3/power | L4 sig+GND. See docs/40."),
        (44, 38.6, "PoE rev2 reserve: isolated PD needs ~22x16 mm incl. 1.5 kV gap; RJ45 must then be a PoE magjack."),
    ]
    for x, y, s in notes:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(s)
        t.SetLayer(pcbnew.Cmts_User)
        t.SetPosition(mm(x, y))
        t.SetTextSize(pcbnew.VECTOR2I_MM(0.8, 0.8))
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
        board.Add(t)

    # board-level minimums (JLC 4-layer standard capability)
    ds.m_MinClearance = pcbnew.FromMM(0.1)
    ds.m_TrackMinWidth = pcbnew.FromMM(0.1)
    ds.m_ViasMinSize = pcbnew.FromMM(0.4)
    ds.m_MinThroughDrill = pcbnew.FromMM(0.2)
    ds.m_CopperEdgeClearance = pcbnew.FromMM(0.3)
    ds.m_HoleToHoleMin = pcbnew.FromMM(0.25)

    pcbnew.SaveBoard(PCB, board)
    inject_stackup(PCB)
    write_project()
    write_dru()
    print("placed %d footprints, %d nets -> %s" % (len(fps), len(netinfo), PCB))


STACKUP = """\t\t(stackup
\t\t\t(layer "F.SilkS" (type "Top Silk Screen"))
\t\t\t(layer "F.Paste" (type "Top Solder Paste"))
\t\t\t(layer "F.Mask" (type "Top Solder Mask") (thickness 0.01))
\t\t\t(layer "F.Cu" (type "copper") (thickness 0.035))
\t\t\t(layer "dielectric 1" (type "prepreg") (thickness 0.0994) (material "3313") (epsilon_r 4.1) (loss_tangent 0.02))
\t\t\t(layer "In1.Cu" (type "copper") (thickness 0.0152))
\t\t\t(layer "dielectric 2" (type "core") (thickness 1.265) (material "FR4") (epsilon_r 4.6) (loss_tangent 0.02))
\t\t\t(layer "In2.Cu" (type "copper") (thickness 0.0152))
\t\t\t(layer "dielectric 3" (type "prepreg") (thickness 0.0994) (material "3313") (epsilon_r 4.1) (loss_tangent 0.02))
\t\t\t(layer "B.Cu" (type "copper") (thickness 0.035))
\t\t\t(layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.01))
\t\t\t(layer "B.Paste" (type "Bottom Solder Paste"))
\t\t\t(layer "B.SilkS" (type "Bottom Silk Screen"))
\t\t\t(copper_finish "ENIG")
\t\t\t(dielectric_constraints no)
\t\t)
"""


def inject_stackup(path):
    s = open(path).read()
    s = re.sub(r"\t\t\(stackup\n.*?\n\t\t\)\n", "", s, flags=re.S)
    s = s.replace("\t(setup\n", "\t(setup\n" + STACKUP, 1)
    open(path, "w").write(s)


# net classes. Widths from the closed-form microstrip model in docs/40 sec. 9 -
# re-check with the JLC impedance calculator for JLC04161H-3313 before ordering.
NETCLASSES = [
    # name, track, clearance, via_d, via_drill, dp_width, dp_gap, patterns
    ("Default", 0.15, 0.15, 0.45, 0.25, 0.15, 0.15, []),
    ("USB_HS_90R", 0.15, 0.15, 0.45, 0.25, 0.15, 0.11, ["USB_D*", "USB_CONN_D*"]),
    ("ETH_MDI_100R", 0.15, 0.2, 0.45, 0.25, 0.15, 0.20, ["MDI_*"]),
    ("RMII_50R", 0.17, 0.15, 0.45, 0.25, 0.17, 0.2,
     ["RMII_*", "PHY_TX*", "PHY_RX*", "PHY_CRS_DV", "REF_CLK_*", "OSC50_OUT", "MDC", "MDIO"]),
    ("CLK_XTAL", 0.15, 0.15, 0.45, 0.25, 0.15, 0.2, ["XTAL_*"]),
    ("POWER", 0.4, 0.15, 0.6, 0.3, 0.2, 0.2,
     ["+3V3", "VDD_HP", "VBUS_C", "VBUS_5V", "PHY_3V3", "VDD_USBPHY", "VDD_BAT", "VDDO_*", "OSC50_VDD", "GND"]),
    ("SW_NODE", 0.5, 0.15, 0.6, 0.3, 0.2, 0.2, ["SW_*"]),
    ("CHASSIS", 0.4, 0.2, 0.6, 0.3, 0.2, 0.2, ["CHASSIS"]),
]


def write_project():
    classes = []
    for i, (n, tw, cl, vd, vdr, dpw, dpg, _) in enumerate(NETCLASSES):
        classes.append({
            "name": n, "bus_width": 12, "clearance": cl, "diff_pair_gap": dpg, "diff_pair_via_gap": 0.25,
            "diff_pair_width": dpw, "line_style": 0, "microvia_diameter": 0.3, "microvia_drill": 0.1,
            "pcb_color": "rgba(0, 0, 0, 0.000)", "priority": 2147483647 if n == "Default" else i,
            "schematic_color": "rgba(0, 0, 0, 0.000)", "track_width": tw, "via_diameter": vd,
            "via_drill": vdr, "wire_width": 6,
        })
    patterns = [{"netclass": n, "pattern": p} for (n, *_rest, pats) in NETCLASSES for p in pats]
    pro = {
        "board": {
            "design_settings": {
                "defaults": {"copper_line_width": 0.2, "copper_text_size_h": 1.0, "copper_text_size_v": 1.0,
                             "copper_text_thickness": 0.15, "silk_line_width": 0.12, "silk_text_size_h": 0.8,
                             "silk_text_size_v": 0.8, "silk_text_thickness": 0.12},
                "rules": {"min_clearance": 0.1, "min_copper_edge_clearance": 0.3, "min_hole_clearance": 0.2,
                          "min_hole_to_hole": 0.25, "min_microvia_diameter": 0.2, "min_microvia_drill": 0.1,
                          "min_resolved_spokes": 1, "min_silk_clearance": 0.0, "min_text_height": 0.6,
                          "min_text_thickness": 0.1, "min_through_hole_diameter": 0.2, "min_track_width": 0.1,
                          "min_via_annular_width": 0.1, "min_via_diameter": 0.4, "solder_mask_to_copper_clearance": 0.0,
                          "use_height_for_length_calcs": True},
                "track_widths": [0.0, 0.1, 0.15, 0.17, 0.25, 0.4, 0.6],
                "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.45, "drill": 0.25},
                                   {"diameter": 0.6, "drill": 0.3}],
                "diff_pair_dimensions": [{"gap": 0.0, "via_gap": 0.0, "width": 0.0},
                                         {"gap": 0.11, "via_gap": 0.25, "width": 0.15},
                                         {"gap": 0.20, "via_gap": 0.25, "width": 0.15}],
            },
        },
        "meta": {"filename": "usb-inferno.kicad_pro", "version": 3},
        "net_settings": {"classes": classes, "meta": {"version": 4}, "netclass_patterns": patterns},
        "pcbnew": {"last_paths": {"netlist": "usb-inferno.net"}},
        "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
    }
    with open(PRO, "w") as f:
        json.dump(pro, f, indent=2)
    with open(os.path.join(KDIR, "fp-lib-table"), "w") as f:
        f.write('(fp_lib_table\n\t(version 7)\n\t(lib (name "usb_inferno") (type "KiCad") (uri "${KIPRJMOD}/../lib/usb_inferno.pretty") (options "") (descr "usb-inferno local"))\n)\n')
    with open(os.path.join(KDIR, "sym-lib-table"), "w") as f:
        f.write('(sym_lib_table\n\t(version 7)\n\t(lib (name "usb_inferno") (type "KiCad") (uri "${KIPRJMOD}/../lib/usb_inferno.kicad_sym") (options "") (descr "usb-inferno local"))\n)\n')


DRU_TEXT = """(version 1)
# usb-inferno rev 1 custom rules (KiCad 9). See docs/40-board-rev1.md section 10.

(rule "USB HS pair skew"
  (condition "A.inDiffPair('USB_D*') || A.inDiffPair('USB_CONN_D*')")
  (constraint skew (max 0.15mm))
  (constraint diff_pair_uncoupled (max 2mm)))

(rule "USB HS keep on top layer, minimal vias"
  (condition "A.NetClass == 'USB_HS_90R'")
  (constraint via_count (max 2)))

(rule "Ethernet MDI pair skew"
  (condition "A.inDiffPair('MDI_T*') || A.inDiffPair('MDI_R*')")
  (constraint skew (max 0.3mm))
  (constraint diff_pair_uncoupled (max 3mm)))

(rule "RMII lines short"
  (condition "A.NetClass == 'RMII_50R'")
  (constraint length (max 40mm)))

(rule "50 MHz REF_CLK branches equal (IP101 6.1)"
  (condition "A.NetName == 'REF_CLK_MAC' || A.NetName == 'REF_CLK_PHY'")
  (constraint length (min 4mm) (max 14mm))
  (constraint via_count (max 1)))

(rule "Crystal traces: no vias (HDG 1.4.2)"
  (condition "A.NetClass == 'CLK_XTAL' || A.NetName == 'XTAL_IN' || A.NetName == 'XTAL_OUT'")
  (constraint via_count (max 0)))

(rule "Chassis isolation"
  (condition "A.NetName == 'CHASSIS' && B.NetName != 'CHASSIS' && !A.memberOfFootprint('J*') && !B.memberOfFootprint('J*')")
  (constraint clearance (min 0.4mm)))
"""


def write_dru():
    with open(DRU, "w") as f:
        f.write(DRU_TEXT)


if __name__ == "__main__":
    main()
