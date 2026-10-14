// SPDX-License-Identifier: GPL-3.0-or-later
//
// usb-inferno UAC2 descriptors. The TinyUSB descriptor callbacks
// (tud_descriptor_*_cb) are implemented in uac2_desc.c.
#pragma once

// Build the serial-number string from the Ethernet MAC. Call before the
// TinyUSB stack starts.
void uac2_desc_init(void);
