#!/usr/bin/env python3
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""Print the ESP32-P4 (U1) and IP101GRI (U6) pin tables, straight from the netlist,
as Markdown. docs/40-board-rev1.md section 5 is generated with this:
    python3 hardware/tools/pin_table.py U1 > /tmp/u1.md
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
from make_pcb import read_netlist  # noqa: E402
from gen_lib import P4_PINS, IP101_PINS  # noqa: E402

NOTES_P4 = {
    "GPIO0": "unused (LP IO, XTAL_32K_N alt.)", "GPIO1": "unused (XTAL_32K_P alt.)",
    "GPIO2": "unused; MTCK, IE+WPU after reset", "GPIO3": "unused; MTDI", "GPIO4": "unused; MTMS",
    "GPIO5": "unused; MTDO",
    "GPIO20": "status LED, active high, 1k",
    "GPIO24": "USB-Serial-JTAG D- -> test pad (FS, prod/debug)",
    "GPIO25": "USB-Serial-JTAG D+ -> test pad; USB_PU after reset",
    "GPIO28": "RMII CRS_DV (22R at PHY)", "GPIO29": "RMII RXD0 (22R at PHY)",
    "GPIO30": "RMII RXD1 (22R at PHY)", "GPIO31": "MDC",
    "GPIO34": "RMII TXD0 (22R at P4). STRAP: JTAG source, only with EFUSE_JTAG_SEL_ENABLE=1 (default ignored)",
    "GPIO35": "RMII TXD1 (22R at P4) + Tag-Connect BOOT. STRAP: boot mode, 10k pull-up -> SPI boot",
    "GPIO36": "STRAP: 10k pull-up (=1, needed for Joint Download when GPIO35=0); test pad",
    "GPIO37": "UART0 TXD -> Tag-Connect. STRAP (GPIO37/38: see TRM; float = any)",
    "GPIO38": "UART0 RXD -> Tag-Connect",
    "GPIO49": "RMII TX_EN (22R at P4)", "GPIO50": "RMII REF_CLK in, 50 MHz from X1 via 22R",
    "GPIO51": "PHY RESET_N, 10k pull-down (PHY held in reset until firmware)",
    "GPIO52": "MDIO, 1.5k pull-up",
    "VDD_HP_1": "pin 54: VDD_HP_1 on rev v3.x (NC on v1.x). Mandatory (PCN202600801)",
    "VDD_MIPI_DPHY": "MIPI unused: may float (HDG 1.3.15)",
    "DSI_REXT": "MIPI unused: float", "CSI_REXT": "MIPI unused: float",
    "USB_DM": "HS OTG D- (0R at chip -> ESD -> USB-C)", "USB_DP": "HS OTG D+ (0R; 1M DNP pull-down)",
    "VDD_USBPHY": "3V3 via 0R, 10n+100n+4.7u",
    "FLASH_CK": "via 0R (+22p DNP) to flash CLK", "FLASH_CS": "10k pull-up to VDDO_FLASH",
    "VDDO_FLASH": "3.3 V default (eFuse 0PXA_TIEH_SEL_0 = 0) -> VDD_FLASHIO + flash VCC, 1u",
    "VDDO_PSRAM": "-> VDD_PSRAM_0/1, 1u", "VDDO_3": "unused LDO out, 1u", "VDDO_4": "unused LDO out, 1u",
    "EN_DCDC": "-> TLV62569 EN (0 V in download mode)", "FB_DCDC": "-> FB node 499k/499k + 22p",
    "CHIP_PU": "RC 10k/1u; Tag-Connect EN", "XTAL_P": "40 MHz via 0R", "XTAL_N": "40 MHz via 0R",
    "VDD_BAT": "3V3 via 0R, 10u + 100n (must not float)", "GND": "exposed pad, 3x3 vias",
}


def main(ref):
    comps, pinnet = read_netlist()
    if ref == "U1":
        rows = sorted(P4_PINS.items())
        print("| pin | name | net | notes |\n|---:|---|---|---|")
        for num, name in rows:
            net = pinnet.get((ref, str(num)), "NC")
            note = NOTES_P4.get(name, "")
            if not note and name.startswith("GPIO") and net == "NC":
                note = "unused, no connect"
            if not note and name.startswith(("DSI_", "CSI_")):
                note = "MIPI unused, no connect"
            if not note and name.startswith(("VDD_IO", "VDD_LP", "VDD_ANA")):
                note = "3V3 + 100n"
            if not note and name.startswith(("VDD_LDO", "VDD_DCDCC")):
                note = "3V3 + 100n + 10u"
            if not note and name.startswith("VDD_HP"):
                note = "VDD_HP + 100n"
            if not note and name.startswith("VDD_PSRAM"):
                note = "VDDO_PSRAM + 100n"
            print("| %d | %s | %s | %s |" % (num, name, net, note))
    else:
        print("| pin | name | net |\n|---:|---|---|")
        for num, name, _t, _s in sorted(IP101_PINS):
            print("| %d | %s | %s |" % (num, name, pinnet.get((ref, str(num)), "NC")))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "U1")
