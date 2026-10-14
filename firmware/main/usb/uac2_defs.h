// SPDX-License-Identifier: GPL-3.0-or-later
//
// usb-inferno UAC2 function: fixed numbers shared by the descriptors
// (uac2_desc.c), the class driver glue (uac2.c) and the clock/position
// engine (uac2_engine.c). See docs/10-usb.md for the layout.
#pragma once

#include "app_config.h"

// ---------------------------------------------------------------------------
// Stream format
// ---------------------------------------------------------------------------
#define UAC2_FRAME_BYTES        (UI_CHANNELS * UI_USB_SUBSLOT)      // 128
#define UAC2_UFRAMES_PER_SEC    8000u                               // HS microframes
#define UAC2_NOMINAL_FPU        (UI_SAMPLE_RATE / UAC2_UFRAMES_PER_SEC)  // 6 frames per microframe
#define UAC2_MAX_FPU            (UAC2_NOMINAL_FPU + 1u)             // 7
// Max packet: (nominal + 1) whole frames. Not "nominal + 4 bytes" (the
// esp_device_uac bug that breaks Windows for > 2 ch).
#define UAC2_EP_DATA_MPS        (UAC2_FRAME_BYTES * UAC2_MAX_FPU)   // 896
#define UAC2_EP_FB_MPS          4u

_Static_assert(UI_SAMPLE_RATE % UAC2_UFRAMES_PER_SEC == 0, "fs must be a multiple of 8 kHz");
_Static_assert(UAC2_EP_DATA_MPS <= 1024, "one HS iso transaction per microframe");

// bInterval (HS: 2^(bInterval-1) microframes)
#define UAC2_EP_DATA_BINTERVAL  1u      // every microframe
#define UAC2_EP_FB_BINTERVAL    4u      // 2^3 = 8 microframes = 1 ms

// ---------------------------------------------------------------------------
// Interfaces / endpoints
// ---------------------------------------------------------------------------
#define UAC2_ITF_AC             0u
#define UAC2_ITF_OUT            1u      // host playback  -> network TX
#define UAC2_ITF_IN             2u      // host capture   <- network RX
#define UAC2_ITF_COUNT          3u

#define UAC2_EP_OUT             0x01u   // iso async data OUT
#define UAC2_EP_FB              0x81u   // iso explicit feedback IN (for EP 0x01)
#define UAC2_EP_IN              0x82u   // iso async data IN

// ---------------------------------------------------------------------------
// Audio function entities
// ---------------------------------------------------------------------------
#define UAC2_ID_IT_USB_OUT      0x01u   // Input Terminal, USB streaming (host playback)
#define UAC2_ID_OT_NET_TX       0x02u   // Output Terminal, digital audio interface (network TX)
#define UAC2_ID_IT_NET_RX       0x03u   // Input Terminal, digital audio interface (network RX)
#define UAC2_ID_OT_USB_IN       0x04u   // Output Terminal, USB streaming (host capture)
#define UAC2_ID_CLOCK           0x09u   // Clock Source, internal fixed 48 kHz

// ---------------------------------------------------------------------------
// Strings (consecutive channel-name indices, as UAC2 iChannelNames requires)
// ---------------------------------------------------------------------------
#define UAC2_STR_LANGID         0u
#define UAC2_STR_MANUFACTURER   1u
#define UAC2_STR_PRODUCT        2u
#define UAC2_STR_SERIAL         3u
#define UAC2_STR_CH_OUT_FIRST   4u                                  // "Net TX 01".."Net TX 32"
#define UAC2_STR_CH_IN_FIRST    (UAC2_STR_CH_OUT_FIRST + UI_CHANNELS) // "Net RX 01".."Net RX 32"
#define UAC2_STR_COUNT          (UAC2_STR_CH_IN_FIRST + UI_CHANNELS)

// ---------------------------------------------------------------------------
// USB identity. 0x303A is Espressif's VID; the PID below is a development
// placeholder and must be replaced by one allocated through Espressif's
// (free) PID programme before the device is distributed.
// ---------------------------------------------------------------------------
#define UAC2_USB_VID            0x303Au
#define UAC2_USB_PID            0x8A32u
#define UAC2_USB_BCD_DEVICE     0x0010u     // tracks UI_FW_VERSION 0.1.0
#define UAC2_MAX_POWER_MA       500u
