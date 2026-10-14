// SPDX-License-Identifier: GPL-3.0-or-later
//
// usb-inferno — fixed configuration. The device has no settings; every
// constant that defines its behaviour lives here.
#pragma once

#include <stdint.h>
#ifndef UAC2_HOST_SIM
#include "sdkconfig.h"
#endif

// ---------------------------------------------------------------------------
// Build-time selections (Kconfig "usb-inferno build profile"; the host
// simulations pass -DUI_CHANNELS / -DUI_LAT_PROFILE instead). The shipped
// device is one fixed build; these exist for the latency/channel test matrix
// (docs/21-latency-matrix.md, tools/build-matrix.sh).
// ---------------------------------------------------------------------------
#define UI_U_(x)                x##u
#define UI_U(x)                 UI_U_(x)        // 32 -> 32u (usable in #if)
#ifndef UI_CHANNELS
#ifdef CONFIG_USBINF_CHANNELS
#define UI_CHANNELS             UI_U(CONFIG_USBINF_CHANNELS)
#else
#define UI_CHANNELS             32u
#endif
#endif
#ifndef UI_LAT_PROFILE
#ifdef CONFIG_USBINF_LAT_PROFILE
#define UI_LAT_PROFILE          CONFIG_USBINF_LAT_PROFILE
#else
#define UI_LAT_PROFILE          10
#endif
#endif

// Cycle-probe instrumentation of the packet paths (sys/perf.h). Default on
// (the bench matrix measures with them); a production image may turn the
// option off once measured (Kconfig USBINF_PERF_PROBES, docs/15 §1).
#if defined(CONFIG_USBINF_PERF_PROBES) && !CONFIG_USBINF_PERF_PROBES
#define UI_PERF_PROBES          0u
#else
#define UI_PERF_PROBES          1u
#endif

// ---------------------------------------------------------------------------
// Audio format (fixed)
// ---------------------------------------------------------------------------
#define UI_SAMPLE_RATE          48000u
// UI_CHANNELS per direction, both USB and network: 2, 4, 8, 16 or 32
#define UI_NET_BITS             24u     // network sample width (on the wire, BE)
#define UI_USB_SUBSLOT          4u      // USB bytes per sample (24-bit valid, LE)
#define UI_USB_BITS             24u

// ---------------------------------------------------------------------------
// Latency profile (frames at UI_SAMPLE_RATE). Values: tools/latency_matrix.py
// PROFILES; budget: docs/14-latency.md. The number is the round-trip target
// in ms with a computer in the loop (10, 6, 4, 2).
//
//   UI_NET_LATENCY_FRAMES  AoIP latency: our RX playout delay and the minimum
//                          we advertise for our TX flows (mDNS latency_ns)
//   UI_FPP_DEFAULT         frames per packet we ask transmitters for / use for
//                          our multicast bundles; also the largest fpp we
//                          advertise (UI_TX_FPP_MAX)

//   UI_USB_OUT_LEAD_FRAMES how far ahead of the media clock USB OUT writes the
//                          TX ring (held by the feedback phase term). Must
//                          cover USB OUT arrival (one microframe, 6) + ISR
//                          latency; see docs/14-latency.md for the jitter
//                          assumption and the engine simulation.
//   UI_USB_IN_MARGIN_FRAMES USB IN reads frame f no earlier than
//                          f + UI_NET_LATENCY_FRAMES + this. Protects the RX
//                          hook's late check (it reads 'now', then writes a
//                          few µs later) against the IN read; 3 frames = 62 µs.
// ---------------------------------------------------------------------------
#if UI_LAT_PROFILE == 10
#define UI_NET_LATENCY_FRAMES   48u     // 1 ms
#define UI_FPP_DEFAULT          16u
#define UI_TX_FPP_MAX           32u     // Inferno's FPP_MAX_ADVERTISED
#define UI_USB_OUT_LEAD_FRAMES  48u     // 1 ms: survives a 1 ms USB ISR stall
#elif UI_LAT_PROFILE == 6
#define UI_NET_LATENCY_FRAMES   48u     // 1 ms
#define UI_FPP_DEFAULT          16u
#define UI_TX_FPP_MAX           32u
#define UI_USB_OUT_LEAD_FRAMES  12u     // 0.25 ms
#elif UI_LAT_PROFILE == 4
#define UI_NET_LATENCY_FRAMES   24u     // 0.5 ms
#define UI_FPP_DEFAULT          8u
#define UI_TX_FPP_MAX           8u
#define UI_USB_OUT_LEAD_FRAMES  12u
#elif UI_LAT_PROFILE == 2
#define UI_NET_LATENCY_FRAMES   12u     // 0.25 ms: direct link / one QoS switch
#define UI_FPP_DEFAULT          4u
#define UI_TX_FPP_MAX           4u
#define UI_USB_OUT_LEAD_FRAMES  12u
#else
#error "UI_LAT_PROFILE must be 10, 6, 4 or 2"
#endif
#ifdef UI_SIM_OUT_LEAD                  // host simulation only: sweep the lead
#undef UI_USB_OUT_LEAD_FRAMES
#define UI_USB_OUT_LEAD_FRAMES  ((unsigned)UI_SIM_OUT_LEAD)
#endif
#define UI_NET_LATENCY_NS       (UI_NET_LATENCY_FRAMES * 125000u / 6u)    // frames * 1e9 / 48000
// The frames->ns conversion truncates: all latency variants must be a
// multiple of 6 frames or the advertised latency is off by up to 20.8 us.
#if (UI_NET_LATENCY_FRAMES % 6u) != 0
#error "UI_NET_LATENCY_FRAMES must be a multiple of 6 (see UI_NET_LATENCY_NS)"
#endif
#define UI_USB_IN_MARGIN_FRAMES 3u
// Loopback test delay (CONFIG_USBINF_TEST_USB_LOOPBACK)
#define UI_LOOPBACK_DELAY_FRAMES 192u

#if UI_CHANNELS != 2 && UI_CHANNELS != 4 && UI_CHANNELS != 8 && UI_CHANNELS != 16 && UI_CHANNELS != 32
#error "UI_CHANNELS must be 2, 4, 8, 16 or 32"
#endif
// (channel sets are uint32_t bitmasks throughout aoip/: never more than 32)

// ---------------------------------------------------------------------------
// Network flows
// ---------------------------------------------------------------------------
#define UI_TX_MAX_CH_PER_FLOW   8u      // as Inferno's MAX_CHANNELS_IN_FLOW
#define UI_TX_MAX_FLOWS         16u     // unicast + multicast flows we transmit
#define UI_RX_MAX_FLOWS         32u     // flows we receive (one per subscribed tx flow)
#define UI_FPP_MIN              2u
#define UI_FPP_MAX              64u     // 8ch*3B*64 = 1536 > MTU: limited per flow by MTU

// ---------------------------------------------------------------------------
// Identity (all derived from the MAC at boot; never configured)
// ---------------------------------------------------------------------------
#define UI_MANUFACTURER         "usb-inferno"
// Model strings follow the build's channel count: 32 -> "UI3232" (unchanged
// for the production build), 8 -> "UI0808", ...
#if UI_CHANNELS == 32
#define UI_CH_STR   "32"
#define UI_CH_STR2  "32"
#elif UI_CHANNELS == 16
#define UI_CH_STR   "16"
#define UI_CH_STR2  "16"
#elif UI_CHANNELS == 8
#define UI_CH_STR   "8"
#define UI_CH_STR2  "08"
#elif UI_CHANNELS == 4
#define UI_CH_STR   "4"
#define UI_CH_STR2  "04"
#else
#define UI_CH_STR   "2"
#define UI_CH_STR2  "02"
#endif
#define UI_MODEL_NAME           "USB-Inferno " UI_CH_STR "x" UI_CH_STR
#define UI_MODEL_ID             "UI" UI_CH_STR2 UI_CH_STR2
#define UI_NAME_PREFIX          UI_MODEL_ID "-"     // + last 3 MAC bytes as hex
#define UI_FW_VERSION           "0.1.0"

// ---------------------------------------------------------------------------
// Board: Waveshare ESP32-P4-ETH
// ---------------------------------------------------------------------------
// IP101 PHY on RMII. REF_CLK 50 MHz enters GPIO50 from outside the chip (never
// the APLL): on the Waveshare board it is the IP101's own 50M_CLKO (PHY on a
// 25 MHz crystal), so it stops while the PHY is held in reset; on the rev 1
// board it is a free-running 50 MHz oscillator (docs/40 section 7.2).
#define UI_PIN_ETH_MDC          31
#define UI_PIN_ETH_MDIO         52
#define UI_PIN_ETH_PHY_RST      51      // PHY reset / power enable
#define UI_PIN_ETH_REF_CLK      50
#define UI_PIN_ETH_TX_EN        49
#define UI_PIN_ETH_TXD0         34
#define UI_PIN_ETH_TXD1         35
#define UI_PIN_ETH_CRS_DV       28
#define UI_PIN_ETH_RXD0         29
#define UI_PIN_ETH_RXD1         30
#define UI_ETH_PHY_ADDR         1
// USB: the HS OTG PHY has dedicated pads (4-pin MX1.25 header on this board,
// wired to a USB-C receptacle pigtail). No GPIO assignment needed.

// ---------------------------------------------------------------------------
// Cores and priorities
// ---------------------------------------------------------------------------
#define UI_CORE_NET             0       // EMAC RX, PTP, TX packetizer
#define UI_CORE_USB             1       // TinyUSB, lwIP, control plane
#define UI_PRIO_EMAC_RX         23
#define UI_PRIO_TX_PACKETIZER   22
// Below the packetizer: the PTP task busy-waits (up to ~1 ms) for the TX DMA
// to drain before each Delay_Req and polls for its TX timestamp. At equal
// priority on core 0 that wait could hold a due audio packet for a tick.
// PTP needs no low latency: every timestamp it uses is taken in hardware.
#define UI_PRIO_PTP             20
#define UI_PRIO_USB             21
#define UI_PRIO_CONTROL         10

// ---------------------------------------------------------------------------
// Telemetry: UDP "key=value" snapshot on request (send any datagram)
// ---------------------------------------------------------------------------
#define UI_TELEM_PORT           7779
