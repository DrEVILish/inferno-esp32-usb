#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""usb-inferno rev 1 - schematic as code (SKiDL -> KiCad 9 netlist + BOM).

One USB-C (UFP, bus powered, USB 2.0 HS audio) + one RJ45 (10/100, IP101GRI RMII)
around an ESP32-P4NRW32X (chip rev v3.x). See docs/40-board-rev1.md for the design
rationale and the source of every value; the bracketed tags below refer to it:

  [DS]   ESP32-P4 Series Datasheet v0.7
  [HDG]  ESP32-P4 Hardware Design Guidelines (master, 2026-10-12), Fig. 1 = v3.x reference schematic
  [REV3] ESP32-P4 Chip Revision v3.x User Guide v1.1 / PCN202600801
  [IP]   IP101G Preliminary Data Sheet IP101G-DS-R01
  [WS]   Waveshare ESP32-P4-ETH schematic (board the firmware runs on today)
  [EVX]  ESP32-P4X-C5-Function-EV-Board v2.0 schematic
  [TPS]  TI TPS25200 SLVSCJ0F;  [TLV] TI TLV62569 SLVSDG1C;  [TPD] TI TPD2EUSB30

Run (from repo root):
  /opt/usb-inferno/work/venv-hw/bin/python hardware/usb_inferno.py
Outputs: hardware/kicad/usb-inferno.net, hardware/bom.csv, hardware/erc.log
"""
import csv
import os
import sys
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
os.environ.setdefault("KICAD9_SYMBOL_DIR", "/usr/share/kicad/symbols")
os.environ.setdefault("KICAD9_FOOTPRINT_DIR", "/usr/share/kicad/footprints")

from skidl import *  # noqa: E402,F403  (NC, POWER, Net, Part, ERC, ... )

set_default_tool(KICAD9)
lib_search_paths[KICAD9].append(os.path.join(HERE, "lib"))

# ---------------------------------------------------------------------------
# BOM catalogue. status: "checked" = MPN/LCSC looked up on lcsc.com/jlcpcb.com
# during this design pass; "recall" = common JLC basic part, number not re-checked
# (verify in the JLC BOM tool); "TBD" = no LCSC listing found.
# ---------------------------------------------------------------------------
PASSIVE = {
    # (kind, value, size): (MPN, manufacturer, LCSC, status, description)
    ("C", "100n", "0402"): ("CL05B104KO5NNNC", "Samsung", "C1525", "recall", "100nF 16V X7R 0402"),
    ("C", "10n", "0402"): ("CL05B103KB5NNNC", "Samsung", "C15195", "recall", "10nF 50V X7R 0402"),
    ("C", "1u", "0402"): ("CL05A105KA5NQNC", "Samsung", "C52923", "recall", "1uF 25V X5R 0402"),
    ("C", "22p", "0402"): ("0402CG220J500NT", "FH", "C1555", "recall", "22pF 50V C0G 0402"),
    ("C", "20p", "0402"): ("0402CG200J500NT", "FH", "C1554", "recall", "20pF 50V C0G 0402"),
    ("C", "4.7u", "0603"): ("CL10A475KO8NNNC", "Samsung", "C19666", "recall", "4.7uF 16V X5R 0603"),
    ("C", "10u", "0603"): ("CL10A106KP8NNNC", "Samsung", "C19702", "recall", "10uF 10V X5R 0603"),
    ("C", "22u", "0805"): ("CL21A226MAQNNNE", "Samsung", "C45783", "recall", "22uF 25V X5R 0805"),
    ("C", "10n/100V", "0603"): ("CL10B103KC8NNNC", "Samsung", "C1589", "recall", "10nF 100V X7R 0603 (chassis bleed)"),
    ("R", "0", "0402"): ("0402WGF0000TCE", "UNI-ROYAL", "C17168", "recall", "0R 0402"),
    ("R", "22", "0402"): ("0402WGF220JTCE", "UNI-ROYAL", "C25092", "recall", "22R 1% 0402"),
    ("R", "1k", "0402"): ("0402WGF1001TCE", "UNI-ROYAL", "C11702", "recall", "1k 1% 0402"),
    ("R", "1.5k", "0402"): ("0402WGF1501TCE", "UNI-ROYAL", "C25867", "recall", "1.5k 1% 0402"),
    ("R", "5.1k", "0402"): ("0402WGF5101TCE", "UNI-ROYAL", "C25905", "recall", "5.1k 1% 0402"),
    ("R", "6.19k", "0402"): ("0402WGF6191TCE", "UNI-ROYAL", "", "TBD", "6.19k 1% 0402 (IP101 ISET)"),
    ("R", "10k", "0402"): ("0402WGF1002TCE", "UNI-ROYAL", "C25744", "recall", "10k 1% 0402"),
    ("R", "22k", "0402"): ("0402WGF2202TCE", "UNI-ROYAL", "C25768", "recall", "22k 1% 0402"),
    ("R", "100k", "0402"): ("0402WGF1003TCE", "UNI-ROYAL", "C25741", "recall", "100k 1% 0402"),
    ("R", "150k", "0402"): ("0402WGF1503TCE", "UNI-ROYAL", "C25755", "recall", "150k 1% 0402"),
    ("R", "499k", "0402"): ("0402WGF4993TCE", "UNI-ROYAL", "", "TBD", "499k 1% 0402 (VDD_HP FB, [HDG] Fig. 5)"),
    ("R", "1M", "0402"): ("0402WGF1004TCE", "UNI-ROYAL", "C26083", "recall", "1M 1% 0402"),
}
FP = {
    ("R", "0402"): "Resistor_SMD:R_0402_1005Metric",
    ("C", "0402"): "Capacitor_SMD:C_0402_1005Metric",
    ("C", "0603"): "Capacitor_SMD:C_0603_1608Metric",
    ("C", "0805"): "Capacitor_SMD:C_0805_2012Metric",
}

BOM = OrderedDict()   # ref -> dict
_count = {}


def _ref(prefix):
    _count[prefix] = _count.get(prefix, 0) + 1
    return "%s%d" % (prefix, _count[prefix])


def bom(part, mpn, mfr, lcsc, status, desc, dnp=False):
    BOM[part.ref] = dict(ref=part.ref, value=part.value, description=desc, mpn=mpn,
                         manufacturer=mfr, lcsc=lcsc, lcsc_status=status,
                         footprint=part.footprint, dnp=dnp)
    if dnp:
        part.fields["DNP"] = "DNP"
    if lcsc:
        part.fields["LCSC"] = lcsc
    if mpn:
        part.fields["MPN"] = mpn


def passive(kind, value, a, b, size="0402", dnp=False, note=""):
    lib_val = value.split("/")[0]
    p = Part("Device", kind, value=lib_val + (" DNP" if dnp else ""), footprint=FP[(kind, size)],
             ref=_ref(kind), tag="%s_%s" % (kind, _count[kind]))
    mpn, mfr, lcsc, st, desc = PASSIVE[(kind, value, size)]
    bom(p, mpn, mfr, lcsc, st, desc + (" - " + note if note else ""), dnp=dnp)
    p[1] += a
    p[2] += b
    return p


def R(value, a, b, **kw):
    return passive("R", value, a, b, **kw)


def C(value, a, b=None, size="0402", **kw):
    return passive("C", value, a, GND if b is None else b, size=size, **kw)


def part(lib, name, ref, footprint, mpn, mfr, lcsc, status, desc, value=None, dnp=False):
    p = Part(lib, name, footprint=footprint, ref=ref, tag=ref)
    if value:
        p.value = value
    if dnp:
        p.value = p.value + " DNP"
    bom(p, mpn, mfr, lcsc, status, desc, dnp=dnp)
    return p


def non_bom(lib, name, ref_prefix, footprint, value=None):
    ref = ref_prefix if ref_prefix[-1].isdigit() else _ref(ref_prefix)
    p = Part(lib, name, footprint=footprint, ref=ref, tag=ref)
    if value:
        p.value = value
    p.fields["exclude_from_bom"] = "yes"
    return p


# ---------------------------------------------------------------------------
# Rails
# ---------------------------------------------------------------------------
def rail(name):
    n = Net(name)
    n.drive = POWER
    return n


GND = rail("GND")
VBUS_C = rail("VBUS_C")          # raw connector VBUS
VBUS_5V = rail("VBUS_5V")        # after eFuse (soft start, current limit, OV clamp)
V3V3 = rail("+3V3")
VDD_HP = rail("VDD_HP")          # P4 core, external buck controlled by P4 EN_DCDC/FB_DCDC
VDD_USBPHY = rail("VDD_USBPHY")
VDD_BAT = rail("VDD_BAT")
PHY_3V3 = rail("PHY_3V3")
CHASSIS = rail("CHASSIS")         # USB shell, RJ45 shield, Bob-Smith node; bled to GND

# ===========================================================================
# 1. USB-C receptacle (UFP sink, USB 2.0 only)
# ===========================================================================
J1 = part("Connector", "USB_C_Receptacle_USB2.0_16P", "J1",
          "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
          "TYPE-C-31-M-12", "Korean Hroparts Elec", "C165948", "checked",
          "USB-C receptacle 16P, USB 2.0, SMD/THT shell", value="USB-C")
USB_DP_C, USB_DN_C = Net("USB_CONN_D+"), Net("USB_CONN_D-")
J1["A4", "A9", "B4", "B9"] += VBUS_C
J1["A1", "A12", "B1", "B12"] += GND
J1["A6", "B6"] += USB_DP_C
J1["A7", "B7"] += USB_DN_C
J1["A8", "B8"] += NC
J1["S1"] += CHASSIS
CC1, CC2 = Net("CC1"), Net("CC2")
J1["A5"] += CC1
J1["B5"] += CC2
R("5.1k", CC1, GND, note="Rd CC1 (UFP)")
R("5.1k", CC2, GND, note="Rd CC2 (UFP)")

# HS-capable ESD on D+/D- at the connector: 0.7 pF typ [TPD], <= 1 pF required [HDG 1.3.12]
D1 = part("Power_Protection", "TPD2EUSB30", "D1", "Package_TO_SOT_SMD:Texas_DRT-3",
          "TPD2EUSB30DRTR", "Texas Instruments", "C97502", "checked",
          "2-ch ESD for USB HS/SS, 0.7 pF")
D1[1] += USB_DP_C
D1[2] += USB_DN_C
D1[3] += GND
# VBUS surge/ESD clamp ahead of the eFuse (eFuse IN withstands 20 V [TPS])
D2 = part("Diode", "SMAJ5.0A", "D2", "Diode_SMD:D_SMA", "SMAJ5.0A", "Littelfuse", "C83329",
          "recall", "TVS 5.0 V standoff, 400 W, SMA")
D2[1] += VBUS_C   # K
D2[2] += GND      # A
C("1u", VBUS_C, note="VBUS input cap; keep total upstream of eFuse < 10 uF (USB 2.0 7.2.4.1)")
C("100n", VBUS_C, note="TPS25200 IN bypass [TPS]")

# Chassis: USB shell + RJ45 shield + magjack Bob-Smith node, bled to GND
R("1M", CHASSIS, GND, note="chassis bleed")
C("10n/100V", CHASSIS, GND, size="0603", note="chassis HF tie (EMC tune)")
R("0", CHASSIS, GND, dnp=True, note="option: hard chassis-GND tie")

# ===========================================================================
# 2. VBUS protection: TPS25200 eFuse (soft start, ILIM, 5.4 V clamp, 7.6 V OVLO)
# ===========================================================================
U2 = part("usb_inferno", "TPS25200DRV", "U2", "Package_SON:WSON-6-1EP_2x2mm_P0.65mm_EP1x1.6mm",
          "TPS25200DRVR", "Texas Instruments", "C128401", "checked",
          "5V eFuse 20V-tolerant, soft start, adj. current limit")
EFUSE_EN, EFUSE_ILIM, EFUSE_FLT = Net("EFUSE_EN"), Net("EFUSE_ILIM"), Net("EFUSE_FAULT_N")
U2[6] += VBUS_C
U2[1] += VBUS_5V
U2[4] += EFUSE_EN
U2[2] += EFUSE_ILIM
U2[3] += EFUSE_FLT
U2[5, 7] += GND
R("100k", VBUS_C, EFUSE_EN, note="EN divider top (EN abs max 7 V)")
R("100k", EFUSE_EN, GND, note="EN divider bottom -> 2.5 V at 5 V")
R("150k", EFUSE_ILIM, GND, note="ILIM 590..710 mA [TPS 5.5 table]")
R("10k", EFUSE_FLT, V3V3, note="FAULT pull-up; FAULT on test pad")
C("10u", VBUS_5V, size="0603", note="eFuse output / buck input bulk")

# ===========================================================================
# 3. 3V3 buck: TLV62569DBV 5 V -> 3.3 V ([EVX] page 6 uses the same part/values)
# ===========================================================================
U3 = part("Regulator_Switching", "TLV62569DBV", "U3", "Package_TO_SOT_SMD:SOT-23-5",
          "TLV62569DBVR", "Texas Instruments", "C141836", "checked", "2A 1.5MHz sync buck")
SW_3V3, FB_3V3 = Net("SW_3V3"), Net("FB_3V3")
U3[4] += VBUS_5V
U3[1] += VBUS_5V
U3[2] += GND
U3[3] += SW_3V3
U3[5] += FB_3V3
C("4.7u", VBUS_5V, size="0603", note="buck CIN [TLV 8.2.2.5]")
L1 = part("Device", "L", "L1", "Inductor_SMD:L_Changjiang_FNR4020S", "FNR4020S2R2MT",
          "cjiang (Changjiang Microelectronics)", "C167825", "checked",
          "2.2uH 3.7A Isat 4x4mm", value="2.2u")
L1[1] += SW_3V3
L1[2] += V3V3
R("100k", V3V3, FB_3V3, note="FB top: 0.6*(1+100/22)=3.327 V [TLV eq.2]")
R("22k", FB_3V3, GND, note="FB bottom")
C("22p", V3V3, FB_3V3, dnp=True, note="optional feed-forward")
C("22u", V3V3, size="0805", note="buck COUT")
C("22u", V3V3, size="0805", note="buck COUT")

# ===========================================================================
# 4. VDD_HP buck: TLV62569DBV 3V3 -> ~1.1 V, EN/FB driven by the P4 [HDG Fig. 5]
# ===========================================================================
U4 = part("Regulator_Switching", "TLV62569DBV", "U4", "Package_TO_SOT_SMD:SOT-23-5",
          "TLV62569DBVR", "Texas Instruments", "C141836", "checked",
          "2A sync buck, P4 VDD_HP (Espressif-verified DCDC)")
SW_HP, EN_DCDC, FB_DCDC = Net("SW_HP"), Net("EN_DCDC"), Net("FB_DCDC")
U4[4] += V3V3
U4[1] += EN_DCDC
U4[2] += GND
U4[3] += SW_HP
U4[5] += FB_DCDC
C("4.7u", V3V3, size="0603", note="VDD_HP buck CIN [HDG Fig. 5]")
L2 = part("Device", "L", "L2", "Inductor_SMD:L_Changjiang_FNR4020S", "FNR4020S2R2MT",
          "cjiang (Changjiang Microelectronics)", "C167825", "checked",
          "2.2uH 3.7A Isat 4x4mm", value="2.2u")
L2[1] += SW_HP
L2[2] += VDD_HP
R("499k", VDD_HP, FB_DCDC, note="must be fitted on v3.x [HDG 1.3.2, REV3 2.1.1]")
R("499k", FB_DCDC, GND, note="must be fitted on v3.x")
C("22p", VDD_HP, FB_DCDC, note="must be fitted on v3.x")
C("22u", VDD_HP, size="0805", note="VDD_HP COUT [HDG Fig. 5]")
C("10u", VDD_HP, size="0603", note="VDD_HP entry bulk [HDG 1.4.1]")

# ===========================================================================
# 5. ESP32-P4NRW32X
# ===========================================================================
U1 = part("usb_inferno", "ESP32-P4NRW32X", "U1",
          "usb_inferno:Espressif_QFN-104-1EP_10x10mm_P0.35mm_EP7.5x7.5mm",
          "ESP32-P4NRW32X", "Espressif Systems", "", "TBD",
          "ESP32-P4 rev v3.x, 32MB PSRAM in package, QFN104")


def P4(name):
    num = {v: k for k, v in LIB_P4.items()}[name]
    return U1[num]


sys.path.insert(0, os.path.join(HERE, "lib"))
from gen_lib import P4_PINS as LIB_P4  # noqa: E402

VDDO_FLASH, VDDO_PSRAM = Net("VDDO_FLASH"), Net("VDDO_PSRAM")
VDDO_3, VDDO_4 = Net("VDDO_3"), Net("VDDO_4")

# --- 3V3 domains, 0.1 uF per pin [HDG 1.3.2 / 1.4.1]
for pin in ("VDD_LP", "VDD_IO_0", "VDD_IO_4", "VDD_IO_5", "VDD_IO_6", "VDD_ANA"):
    P4(pin).__iadd__(V3V3)
    C("100n", V3V3, note="decap " + pin)
C("10u", V3V3, size="0603", note="3V3 IO-domain entry bulk [HDG 1.4.1]")
for pin in ("VDD_LDO", "VDD_DCDCC"):
    P4(pin).__iadd__(V3V3)
    C("100n", V3V3, note="decap " + pin)
    C("10u", V3V3, size="0603", note="bulk at " + pin + " [HDG 1.3.2]")
# VDD_BAT: 3V3 via 0R, 10 uF + 0.1 uF, must not float [HDG 1.3.2, Fig. 1 R5]
R("0", V3V3, VDD_BAT, note="VDD_BAT feed")
P4("VDD_BAT").__iadd__(VDD_BAT)
C("10u", VDD_BAT, size="0603", note="VDD_BAT")
C("100n", VDD_BAT, note="VDD_BAT")
# VDD_USBPHY: 3V3 via 0R (kept for compatibility on v3.x), 10n+100n+4.7u [HDG USB PHY Power]
R("0", V3V3, VDD_USBPHY, note="VDD_USBPHY feed (0R footprint per HDG)")
P4("VDD_USBPHY").__iadd__(VDD_USBPHY)
C("10n", VDD_USBPHY, note="VDD_USBPHY")
C("100n", VDD_USBPHY, note="VDD_USBPHY")
C("4.7u", VDD_USBPHY, size="0603", note="VDD_USBPHY")
# VDD_HP_0..3 (pin 54 = VDD_HP_1 on v3.x, mandatory: PCN202600801 says USB/SDIO/GMAC fail otherwise)
for pin in ("VDD_HP_0", "VDD_HP_1", "VDD_HP_2", "VDD_HP_3"):
    P4(pin).__iadd__(VDD_HP)
    C("100n", VDD_HP, note="decap " + pin)
P4("EN_DCDC").__iadd__(EN_DCDC)
P4("FB_DCDC").__iadd__(FB_DCDC)
# LDO outputs, 1 uF each [HDG Fig. 1 C17/C18/C19/C22]
P4("VDDO_FLASH").__iadd__(VDDO_FLASH)
P4("VDD_FLASHIO").__iadd__(VDDO_FLASH)
C("1u", VDDO_FLASH, note="VDDO_FLASH")
C("100n", VDDO_FLASH, note="VDD_FLASHIO")
P4("VDDO_PSRAM").__iadd__(VDDO_PSRAM)
P4("VDD_PSRAM_0").__iadd__(VDDO_PSRAM)
P4("VDD_PSRAM_1").__iadd__(VDDO_PSRAM)
C("1u", VDDO_PSRAM, note="VDDO_PSRAM")
C("100n", VDDO_PSRAM, note="VDD_PSRAM_0")
C("100n", VDDO_PSRAM, note="VDD_PSRAM_1")
P4("VDDO_3").__iadd__(VDDO_3)
C("1u", VDDO_3, note="VDDO_3 (unused LDO, cap per HDG)")
P4("VDDO_4").__iadd__(VDDO_4)
C("1u", VDDO_4, note="VDDO_4 (unused LDO, cap per HDG)")
P4("GND").__iadd__(GND)
# MIPI unused: supply and REXT may float [HDG 1.3.15]
for pin in ("VDD_MIPI_DPHY", "DSI_REXT", "CSI_REXT", "DSI_DATAP1", "DSI_DATAN1", "DSI_CLKN",
            "DSI_CLKP", "DSI_DATAP0", "DSI_DATAN0", "CSI_DATAN0", "CSI_DATAP0", "CSI_CLKP",
            "CSI_CLKN", "CSI_DATAN1", "CSI_DATAP1"):
    P4(pin).__iadd__(NC)

# --- CHIP_PU RC reset 10k / 1uF [HDG 1.3.3]
CHIP_PU = Net("CHIP_PU")
P4("CHIP_PU").__iadd__(CHIP_PU)
R("10k", V3V3, CHIP_PU, note="CHIP_PU RC")
C("1u", CHIP_PU, note="CHIP_PU RC (tSTBL >= 50 us, HDG 1.3.3)")

# --- 40 MHz crystal, +-10 ppm, 0R series footprints [HDG 1.3.5, Fig. 1 R3/R4]
Y1 = part("Device", "Crystal_GND24", "Y1", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm",
          "X322540MPB4SI", "YXC", "C9010", "checked", "40MHz 15pF +-10ppm 3225", value="40MHz")
XIN, XOUT, XTAL_P, XTAL_N = Net("XTAL_IN"), Net("XTAL_OUT"), Net("XTAL_P"), Net("XTAL_N")
Y1[1] += XIN
Y1[3] += XOUT
Y1[2, 4] += GND
C("22p", XIN, note="CL 15 pF: (22/2)+~4 pF stray; tune on first build [HDG 1.3.5]")
C("22p", XOUT, note="crystal load")
R("0", XTAL_P, XIN, note="XTAL_P series (place at chip)")
R("0", XTAL_N, XOUT, note="XTAL_N series (place at chip)")
P4("XTAL_P").__iadd__(XTAL_P)
P4("XTAL_N").__iadd__(XTAL_N)

# --- QSPI flash 16 MB, 3.3 V, powered by VDDO_FLASH [DS 2.7, HDG 1.3.4]
U5 = part("Memory_Flash", "W25Q128JVS", "U5", "Package_SO:SOIC-8_5.3x5.3mm_P1.27mm",
          "W25Q128JVSIQ", "Winbond", "C97521", "checked", "128Mbit QSPI NOR 3.3V SOIC-8 208mil")
F_CS, F_Q, F_WP, F_HD, F_CK, F_CKP, F_D = (Net(n) for n in (
    "FLASH_CS", "FLASH_Q", "FLASH_WP", "FLASH_HOLD", "FLASH_CK", "FLASH_CK_P4", "FLASH_D"))
P4("FLASH_CS").__iadd__(F_CS)
P4("FLASH_Q").__iadd__(F_Q)
P4("FLASH_WP").__iadd__(F_WP)
P4("FLASH_HOLD").__iadd__(F_HD)
P4("FLASH_CK").__iadd__(F_CKP)
P4("FLASH_D").__iadd__(F_D)
U5[1] += F_CS
U5[2] += F_Q
U5[3] += F_WP
U5[7] += F_HD
U5[6] += F_CK
U5[5] += F_D
U5[8] += VDDO_FLASH
U5[4] += GND
R("0", F_CKP, F_CK, note="FLASH_CK series (EMC option) [HDG 1.3.4/1.3.7]")
C("22p", F_CK, dnp=True, note="FLASH_CK EMC option [HDG 1.3.7]")
R("10k", VDDO_FLASH, F_CS, note="FLASH_CS pull-up [HDG 1.3.4]")
C("100n", VDDO_FLASH, note="flash VCC")

# --- USB 2.0 HS OTG (UAC2 device): dedicated pads, 0R near chip [HDG 1.4.3]
USB_DP, USB_DN = Net("USB_D+"), Net("USB_D-")
P4("USB_DP").__iadd__(USB_DP)
P4("USB_DM").__iadd__(USB_DN)
R("0", USB_DP_C, USB_DP, note="HS D+ series option, at chip")
R("0", USB_DN_C, USB_DN, note="HS D- series option, at chip")
R("1M", USB_DP, GND, dnp=True, note="v1.x DP pull-down; optional on v3.x [HDG 1.3.12]")

# --- USB-Serial-JTAG (FS, GPIO24 D-/GPIO25 D+) to test pads only [HDG 1.3.12]
USJ_DN, USJ_DP = Net("USJ_DM"), Net("USJ_DP")
P4("GPIO24").__iadd__(USJ_DN)
P4("GPIO25").__iadd__(USJ_DP)

# --- UART0 (GPIO37 TX, GPIO38 RX) [HDG 1.3.6] and straps
U0TXD, U0RXD = Net("U0TXD"), Net("U0RXD")
P4("GPIO37").__iadd__(U0TXD)
P4("GPIO38").__iadd__(U0RXD)
BOOT = Net("RMII_TXD1")            # GPIO35 = boot strap AND RMII TXD1
STRAP36 = Net("GPIO36")
P4("GPIO36").__iadd__(STRAP36)
R("10k", V3V3, BOOT, note="GPIO35 boot strap pull-up (=1 SPI boot) [HDG 1.3.8]")
R("10k", V3V3, STRAP36, note="GPIO36=1 so BOOT-low gives Joint Download [DS Table 3-3]")

# --- status LED (firmware-driven, active high)
LED_ST = Net("LED_STATUS")
LED_ST_A = Net("LED_STATUS_A")
P4("GPIO20").__iadd__(LED_ST)
R("1k", LED_ST, LED_ST_A, note="status LED ~1 mA")
D3 = part("Device", "LED", "D3", "LED_SMD:LED_0603_1608Metric", "19-217/GHC-YR1S2/3T", "Everlight",
          "C72043", "recall", "LED green 0603", value="green")
D3[2] += LED_ST_A
D3[1] += GND

# --- RMII to IP101GRI (pin map = firmware app_config.h, unchanged)
RMII = OrderedDict([
    ("GPIO28", "RMII_CRS_DV"), ("GPIO29", "RMII_RXD0"), ("GPIO30", "RMII_RXD1"),
    ("GPIO31", "MDC"), ("GPIO34", "RMII_TXD0"), ("GPIO49", "RMII_TX_EN"),
    ("GPIO50", "REF_CLK_MAC"), ("GPIO51", "PHY_RST_N"), ("GPIO52", "MDIO"),
])
RN = {v: Net(v) for v in RMII.values()}
RN["RMII_TXD1"] = BOOT
for g, n in RMII.items():
    P4(g).__iadd__(RN[n])
P4("GPIO35").__iadd__(BOOT)

# every other GPIO unconnected
used = set(RMII) | {"GPIO35", "GPIO36", "GPIO37", "GPIO38", "GPIO24", "GPIO25", "GPIO20"}
for num, name in LIB_P4.items():
    if name.startswith("GPIO") and name not in used:
        U1[num] += NC

# ===========================================================================
# 6. 50 MHz RMII reference clock: oscillator -> P4 GPIO50 and PHY 50M_CLKI,
#    star with one 22R per load, equal lengths [IP 6.1, Table 3]
# ===========================================================================
X1 = part("Oscillator", "ASE-xxxMHz", "X1", "Oscillator:Oscillator_SMD_Abracon_ASE-4Pin_3.2x2.5mm",
          "ECS-3225MV-500-CN-TR", "ECS Inc.", "C405974", "checked",
          "50MHz CMOS XO 1.62-3.63V +-25ppm 3225 (clock option A; remove for B)", value="50MHz")
OSC_OUT, OSC_VDD = Net("OSC50_OUT"), rail("OSC50_VDD")
REF_CLK_PHY = Net("REF_CLK_PHY")
X1[4] += OSC_VDD
X1[1] += OSC_VDD
X1[2] += GND
X1[3] += OSC_OUT
R("0", PHY_3V3, OSC_VDD, note="osc supply (fit ferrite/10R if spurs)")
C("100n", OSC_VDD, note="osc decap")
C("1u", OSC_VDD, note="osc decap")
R("22", OSC_OUT, RN["REF_CLK_MAC"], note="REF_CLK to P4 GPIO50 [HDG 1.3.14 series R]")
R("22", OSC_OUT, REF_CLK_PHY, note="REF_CLK to PHY 50M_CLKI")

# ===========================================================================
# 7. IP101GRI PHY  [IP pin table, WS wiring]
# ===========================================================================
U6 = part("usb_inferno", "IP101GRI", "U6", "Package_DFN_QFN:QFN-32-1EP_4x4mm_P0.4mm_EP2.65x2.65mm_ThermalVias",
          "IP101GRI", "IC Plus", "C703537", "checked", "10/100 Ethernet PHY RMII QFN32 4x4")
R("0", V3V3, PHY_3V3, note="PHY supply (0R as WS R71; ferrite option)")
PHY = {n: Net(n) for n in ("PHY_TXEN", "PHY_TXD0", "PHY_TXD1", "PHY_RXD0", "PHY_RXD1", "PHY_CRS_DV",
                           "PHY_TXER", "PHY_COL_RMII", "PHY_LEDMOD", "PHY_LED0_AD0", "PHY_LED3_AD3",
                           "PHY_ISET", "PHY_REGOUT", "MDI_TP", "MDI_TN", "MDI_RP", "MDI_RN")}
U6[1] += PHY["PHY_TXER"]
PHY_X1, PHY_X2, PHY_CLKO = Net("PHY_X1"), Net("PHY_X2"), Net("PHY_50M_CLKO")
U6[2] += PHY_X1                   # X1: GND in 50 MHz-input mode (R fitted), crystal in fallback
U6[3] += PHY_X2                   # X2: open in 50 MHz mode, crystal in fallback
U6[4] += PHY["PHY_COL_RMII"]
U6[5] += PHY["PHY_TXEN"]
U6[6, 7] += GND                   # TXD3/TXD2 unused in RMII: tied low (WS leaves open)
U6[8] += PHY["PHY_TXD1"]
U6[9] += PHY["PHY_TXD0"]
U6[10] += REF_CLK_PHY
U6[11] += PHY["PHY_LED0_AD0"]
U6[12] += PHY["PHY_LED3_AD3"]
U6[13] += PHY_3V3                 # VDDIO (IP101GRI: 3.3 V only)
U6[14] += PHY_CLKO                # 50M_CLKO: used only in the 25 MHz-crystal fallback
U6[15, 16] += NC                  # RXD3/RXD2
U6[17] += PHY["PHY_RXD1"]
U6[18] += PHY["PHY_RXD0"]
U6[19] += PHY["PHY_CRS_DV"]
U6[20] += PHY["PHY_LEDMOD"]
U6[21] += NC                      # RXER/INTR unused
U6[22] += RN["MDC"]
U6[23] += RN["MDIO"]
U6[24] += NC                      # TEST_ON internal pull-down
U6[25] += PHY["PHY_ISET"]
U6[26] += PHY["MDI_RN"]
U6[27] += PHY["MDI_RP"]
U6[28] += PHY["PHY_REGOUT"]
U6[29] += PHY["MDI_TN"]
U6[30] += PHY["MDI_TP"]
U6[31] += PHY_3V3                 # AVDD33
U6[32] += RN["PHY_RST_N"]
U6[33] += GND
# --- RMII clock option, selected by population only (docs/40 7.2):
#  A (default): X1 50 MHz XO fitted, R "X1 to GND" fitted; Y2, its caps and R_CLKO DNP.
#  B (fallback, = [WS]/[EVX]): X1 XO removed, R "X1 to GND" removed; fit Y2 25 MHz + 2x20p
#    and R_CLKO 22R: PHY 50M_CLKO -> OSC50_OUT star -> 22R -> GPIO50 and 22R -> PHY 50M_CLKI.
R("0", PHY_X1, GND, note="CLK option A: X1 to GND (50 MHz into 50M_CLKI) [IP Table 3]; remove for B")
Y2 = part("Device", "Crystal_GND24", "Y2", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm",
          "X322525MSB4SI", "YXC", "C13740", "checked",
          "25MHz 3225 crystal, PHY clock option B (fit for B)", value="25MHz", dnp=True)
Y2[1] += PHY_X1
Y2[3] += PHY_X2
Y2[2, 4] += GND
C("20p", PHY_X1, dnp=True, note="option B crystal load ([WS] C113 20 pF)")
C("20p", PHY_X2, dnp=True, note="option B crystal load ([WS] C120 20 pF)")
R("22", PHY_CLKO, OSC_OUT, dnp=True, note="option B: PHY 50M_CLKO into the REF_CLK star (fit for B)")

# series 22R at the driver end of every RMII data line
R("22", RN["RMII_TX_EN"], PHY["PHY_TXEN"], note="at P4")
R("22", RN["RMII_TXD0"], PHY["PHY_TXD0"], note="at P4")
R("22", BOOT, PHY["PHY_TXD1"], note="at P4 (GPIO35 strap stays on P4 side)")
R("22", PHY["PHY_RXD0"], RN["RMII_RXD0"], note="at PHY")
R("22", PHY["PHY_RXD1"], RN["RMII_RXD1"], note="at PHY")
R("22", PHY["PHY_CRS_DV"], RN["RMII_CRS_DV"], note="at PHY")
# straps: 5.1 kΩ per [IP Table 12]
R("5.1k", PHY["PHY_TXER"], GND, note="TXER low in RMII [IP pin 1]")
R("5.1k", PHY_3V3, PHY["PHY_COL_RMII"], note="COL/RMII=1 -> RMII")
R("5.1k", PHY_3V3, PHY["PHY_LEDMOD"], note="LEDMOD=1 -> LED mode 2")
R("5.1k", PHY_3V3, PHY["PHY_LED0_AD0"], note="PHY_AD0=1 -> PHY address 1 (firmware)")
R("5.1k", PHY["PHY_LED3_AD3"], GND, note="PHY_AD3=0")
R("1.5k", PHY_3V3, RN["MDIO"], note="MDIO pull-up (WS R59)")
R("10k", RN["PHY_RST_N"], GND, note="hold PHY in reset until firmware releases it")
R("6.19k", PHY["PHY_ISET"], GND, note="ISET 6.19k 1% [IP pin 25]")
C("10u", PHY["PHY_REGOUT"], size="0603", note="REGOUT [IP pin 28]")
C("100n", PHY["PHY_REGOUT"], note="REGOUT")
C("100n", PHY_3V3, note="VDDIO")
C("100n", PHY_3V3, note="AVDD33")
C("10u", PHY_3V3, size="0603", note="PHY bulk")

# ===========================================================================
# 8. Magjack HR911105A (1:1 transformers, 2 LEDs)
# ===========================================================================
J2 = part("Connector", "RJ45_Hanrun_HR911105A_Horizontal", "J2", "Connector_RJ:RJ45_Hanrun_HR911105A_Horizontal",
          "HR911105A", "HanRun", "C12074", "checked", "RJ45 magjack 10/100 with LEDs", value="RJ45")
TCT, RCT = Net("MAG_TCT"), Net("MAG_RCT")
J2[1] += PHY["MDI_TP"]
J2[2] += PHY["MDI_TN"]
J2[3] += PHY["MDI_RP"]
J2[6] += PHY["MDI_RN"]
J2[4] += TCT
J2[5] += RCT
J2[7] += NC
J2[8] += CHASSIS                  # Bob-Smith 1000 pF node
J2["SH"] += CHASSIS
C("100n", TCT, note="TX centre tap: decoupling only, no supply [IP Table 1, 6.2]")
C("100n", RCT, note="RX centre tap: decoupling only")
# LEDs (polarity from KiCad symbol: 12/11 = yellow A/K, 9/10 = green A/K) - verify vs Hanrun DS
LEDY_K, LEDG_K = Net("MAG_LEDY_K"), Net("MAG_LEDG_K")
J2[12] += PHY_3V3                 # yellow anode
J2[11] += LEDY_K
R("1k", LEDY_K, PHY["PHY_LED0_AD0"], note="LED0 (link/act, active low; AD0 strapped high)")
J2[9] += PHY["PHY_LED3_AD3"]      # green anode (LED3 100M, active high; AD3 strapped low)
J2[10] += LEDG_K
R("1k", LEDG_K, GND, note="LED3 current limit")

# ===========================================================================
# 9. Programming / debug: TC2030-IDC-NL + test pads
# ===========================================================================
J3 = non_bom("Connector", "TC2030", "J3", "Connector:Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical", value="TC2030")
J3[1] += V3V3
J3[2] += CHIP_PU
J3[3] += GND
J3[4] += U0TXD
J3[5] += U0RXD
J3[6] += BOOT
for net, label in ((USJ_DP, "USJ_D+"), (USJ_DN, "USJ_D-"), (VBUS_5V, "5V"), (V3V3, "3V3"),
                   (VDD_HP, "VDD_HP"), (GND, "GND"), (EFUSE_FLT, "FAULT"), (STRAP36, "IO36")):
    tp = non_bom("Connector", "TestPoint", "TP", "TestPoint:TestPoint_Pad_D1.0mm", value=label)
    tp[1] += net

# mounting holes (M2, plated, to CHASSIS)
for _ in range(4):
    mh = non_bom("Mechanical", "MountingHole_Pad", "H", "MountingHole:MountingHole_2.2mm_M2_Pad_Via", value="M2")
    mh[1] += CHASSIS


# ===========================================================================
# outputs
# ===========================================================================
def write_bom(path):
    groups = OrderedDict()
    for b in BOM.values():
        key = (b["value"], b["mpn"], b["footprint"], b["dnp"])
        groups.setdefault(key, []).append(b)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ref", "value", "description", "MPN", "manufacturer", "LCSC", "LCSC_status",
                    "footprint", "qty", "dnp"])
        for (val, mpn, fp, dnp), items in sorted(groups.items(), key=lambda kv: (kv[1][0]["ref"][0], kv[0][0])):
            refs = sorted((i["ref"] for i in items), key=lambda r: (r.rstrip("0123456789"), int(r[len(r.rstrip("0123456789")):])))
            b = items[0]
            w.writerow([" ".join(refs), val, b["description"].split(" - ")[0], mpn, b["manufacturer"],
                        b["lcsc"], b["lcsc_status"], fp, len(items), "DNP" if dnp else ""])


if __name__ == "__main__":
    out = os.path.join(HERE, "kicad", "usb-inferno.net")
    ERC()
    generate_netlist(file_=out)
    write_bom(os.path.join(HERE, "bom.csv"))
    print("netlist:", out)
