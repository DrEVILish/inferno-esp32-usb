// SPDX-License-Identifier: GPL-3.0-or-later
//
// CMC server (port 8800). Translated from Inferno device_server/cmc_server.rs
// and protocol/proto_cmc.rs. The controller's first unicast to a new device
// is the device-advertisement request; unanswered, it never talks ARC.
#include <string.h>

#include "aoip/aoip_internal.h"
#include "aoip/proto.h"
#include "aoip/wire.h"

size_t cmc_handle(const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    if (len < RR_HEADER_LEN || rd16(req + 8) != 0) return 0;
    if (rd16(req + 6) != CMC_OP_REQUEST_DEVICE_ADV) return 0;
    if (cap < RR_HEADER_LEN + CMC_SZ_DEVICE_ADV) return 0;

    // DeviceAdvertisement: u16 process_id, [8] factory_device_id, u16 1,
    // u16 0, [4] ip, u16 info_request_port, u16 0
    uint8_t *c = out + RR_HEADER_LEN;
    uint32_t ip = atomic_load(&g_aoip.ip);
    wr16(c + 0, g_aoip.process_id);
    memcpy(c + 2, g_aoip.devid, 8);
    wr16(c + 10, 1);
    wr16(c + 12, 0);
    memcpy(c + 14, &ip, 4);
    wr16(c + 18, AOIP_PORT_INFO_REQUEST);
    wr16(c + 20, 0);
    const size_t total = RR_HEADER_LEN + CMC_SZ_DEVICE_ADV;
    rr_finish(out, req, RR_CODE_OK, (uint16_t)total);
    return total;
}
