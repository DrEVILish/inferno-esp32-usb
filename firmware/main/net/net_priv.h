// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal interface between net/ and clock/ (both owned by the net+clock
// module). Not for use by usb/ or aoip/.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_eth_driver.h"
#include "net/net.h"

// Ethernet driver handle (NULL before net_start()).
esp_eth_handle_t net_eth_handle(void);

// True once the EMAC's IEEE 1588 unit has been enabled and configured by
// net_start() (digital rollover, PTPv1-over-UDP event classifier).
bool net_ptp_hw_ready(void);

// Send one UDP datagram on the raw path and return its hardware TX
// timestamp in ns (PTP time), or 0 when the MAC did not report one. Waits
// (bounded, ~1 ms) for the TX DMA to go idle first, so the frame does not
// queue behind audio and the driver's bounded timestamp poll succeeds.
// Low rate only (PTP Delay_Req): it busy-waits for the timestamp.
esp_err_t net_udp_send_raw_ts(const net_udp_dest_t *d, const void *payload,
                              size_t len, uint64_t *tx_ts_ns);
