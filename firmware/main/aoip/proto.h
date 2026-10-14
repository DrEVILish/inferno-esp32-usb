// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wire constants of the Inferno-compatible AoIP protocol.
//
// Every value below is copied from Inferno (inferno_aoip/src, GPL/AGPL, used
// as the protocol specification). The file each one comes from is named next
// to it. Values Inferno itself marks as unknown ("???", "TODO", "WTF") are
// copied verbatim and keep that mark here. Nothing in this file is invented;
// the only values that are our own choice are in the last section and are
// local port numbers, not protocol constants.
#pragma once

#include <stdint.h>
#include "app_config.h"

// ---------------------------------------------------------------------------
// Ports (protocol/proto_arc.rs, proto_cmc.rs, flows_control.rs, mcast.rs,
// device_server/info_mcast_server.rs, tx_multicasts.rs)
// ---------------------------------------------------------------------------
#define AOIP_PORT_ARC               4440u
#define AOIP_PORT_CMC               8800u
#define AOIP_PORT_FLOWS_CONTROL     4455u
#define AOIP_PORT_INFO_REQUEST      8700u
#define AOIP_PORT_INFO_DST          8702u   // DST_PORT_DEVICE_INFO
#define AOIP_PORT_HEARTBEAT_DST     8708u   // DST_PORT_HEARTBEAT
#define AOIP_PORT_MEDIA_MCAST       4321u   // tx_multicasts.rs MEDIA_PORT
// 224.0.0.231 and 224.0.0.233, host byte order
#define AOIP_MCAST_DEVICE_INFO      0xE00000E7u
#define AOIP_MCAST_HEARTBEAT        0xE00000E9u

// ---------------------------------------------------------------------------
// Request/response framing (protocol/req_resp.rs)
//   u16 start_code, u16 total_length, u16 seqnum, u16 opcode1, u16 opcode2,
//   content...
// ---------------------------------------------------------------------------
#define RR_HEADER_LEN               10u
#define RR_CODE_OK                  1u
#define RR_CODE_TODO_ERROR          0xFFFFu // Inferno: "0xFFFF /* TODO */"

// ---------------------------------------------------------------------------
// ARC (protocol/proto_arc.rs, device_server/arc_server.rs)
// ---------------------------------------------------------------------------
#define ARC_PACKET_SIZE_SOFT_LIMIT  800u
#define ARC_CODE_HAVE_MORE          0x8112u // paginate_make_response

#define ARC_OP_CHANNELS_FLOWS_COUNT 0x1000u
#define ARC_OP_GET_DEVICE_NAME      0x1002u
#define ARC_OP_GET_DEVICE_NAMES     0x1003u
#define ARC_OP_GET_TX_CHANNELS      0x2000u
#define ARC_OP_GET_TX_FRIENDLY      0x2010u
#define ARC_OP_RENAME_TX_CHANNELS   0x2013u
#define ARC_OP_QUERY_TX_FLOWS       0x2200u
#define ARC_OP_CREATE_MCAST_TX_FLOW 0x2201u
#define ARC_OP_DELETE_MCAST_TX_FLOW 0x2202u
#define ARC_OP_UNKNOWN_2320         0x2320u // "???" -> respond code 0x30, empty
#define ARC_OP_GET_RX_CHANNELS      0x3000u
#define ARC_OP_RENAME_RX_CHANNELS   0x3001u
#define ARC_OP_SET_SUBSCRIPTIONS    0x3010u
#define ARC_OP_NETAUDIO_UNSUBSCRIBE 0x3014u // used by network-audio-controller
#define ARC_OP_QUERY_RX_FLOWS       0x3200u
#define ARC_OP_DEVICE_SETTINGS        0x1100u // device settings (rate, latency)
#define ARC_OP_DEVICE_SETTINGS_WRITE  0x1101u // refused (0x30): latency is build-time, not settable
#define ARC_OP_PROPERTY_DIRECTORY     0x1102u // property id/flags directory
#define ARC_OP_TX_FLOW_LABELS         0x2204u // empty page: the device keeps no flow labels
#define ARC_OP_UNKNOWN_3300         0x3300u // "WTF: necessary to avoid 'clock domain mismatch'"
// NOT in Inferno: device rename as sent by network-audio-controller
// ("netaudio device config --name"), content = new name, NUL-terminated;
// empty content = reset to the default name. Unverified, see docs/12-aoip.md.
#define ARC_OP_SET_DEVICE_NAME      0x1001u

#define ARC_CODE_2320               0x30u
#define ARC_CODE_SETTINGS_REFUSED   0x30u // 0x1101 write: latency is not settable on this device
#define ARC_3300_REPLY              { 0x38, 0x00, 0x38, 0xfd, 0x38, 0xfe, 0x38, 0xff }
#define ARC_DEVICE_NAMES_START_CODE 0x2729u
#define ARC_DEVICE_NAMES_OP_1102    0x1102u
#define ARC_REVISION_STRING         ":705"  // written with its NUL

// Serialized sizes of the binary_serde structs (big endian, packed)
#define ARC_SZ_CHANNELS_FLOWS_COUNT 34u
#define ARC_SZ_DEVICE_NAMES_HDR     38u
#define ARC_SZ_COMMON_CH_DESCR      16u
#define ARC_SZ_RX_CH_DESCR          20u
#define ARC_SZ_TX_CH_DESCR          8u
#define ARC_SZ_TX_FRIENDLY_DESCR    6u
#define ARC_SZ_RENAME_TX_REQ        6u
#define ARC_SZ_RENAME_RX_REQ        4u
#define ARC_SZ_DEST_SOCKET_DESCR    8u
#define ARC_SZ_TXF_NAMES_DESCR      20u
#define ARC_SZ_TXF_HEADER           18u
#define ARC_SZ_MCAST_CREATE_HEADER  16u
#define ARC_SZ_MCAST_CREATE_FOOTER  2u
#define ARC_SZ_RXF_DESCR2           16u
#define ARC_SZ_RXF_HEADER           20u
#define ARC_SZ_SUBSCRIPTION_REQ     6u

#define ARC_UNKNOWN_DEST_8002       0x8002u // DestinationSocketDescriptor.unknown1_8002
#define ARC_TXF_TYPE_UNICAST        0x11u
#define ARC_TXF_TYPE_MULTICAST      2u

// SubscriptionStatus (channels_subscriber.rs)
#define SUB_STATUS_NONE             0u
#define SUB_STATUS_UNRESOLVED       1u
#define SUB_STATUS_IN_PROGRESS      8u
#define SUB_STATUS_TOO_MANY_TX      0x14u
#define SUB_STATUS_TX_FAIL          0x15u
#define SUB_STATUS_RX_UNICAST       0x01010009u
#define SUB_STATUS_RX_MULTICAST     0x0101000au

// ---------------------------------------------------------------------------
// CMC (protocol/proto_cmc.rs)
// ---------------------------------------------------------------------------
#define CMC_OP_REQUEST_DEVICE_ADV   0x1001u
#define CMC_SZ_DEVICE_ADV           22u

// ---------------------------------------------------------------------------
// Info multicast (protocol/mcast.rs, device_server/info_mcast_server.rs)
//   u16 start_code, u16 total_length, u16 seqnum, u16 process,
//   [8] factory_device_id, [8] vendor, [8] opcode, content...
// ---------------------------------------------------------------------------
#define MC_HEADER_LEN               32u
#define MC_START_CODE_INFO          0xffffu
#define MC_START_CODE_HEARTBEAT     0xfffeu
#define MC_OP_BOARD_INFO            { 0x07, 0x2a, 0x00, 0x60, 0, 0, 0, 0 }
#define MC_OP_PRODUCT_INFO          { 0x07, 0x2a, 0x00, 0xc0, 0, 0, 0, 0 }
#define MC_OP_CLOCK_STATS           { 0x07, 0x2a, 0x00, 0x20, 0, 0, 0, 0 }
#define MC_OP_NETWORK_INFO          { 0x07, 0x2a, 0x00, 0x11, 0, 0, 0, 0 }
#define MC_OP_REPLY_78              { 0x07, 0x2a, 0x00, 0x78, 0, 0, 0, 0 }
#define MC_OP_CHANNEL_CHANGE        { 0x07, 0x2a, 0x01, 0x02, 0, 0, 0, 0 }
#define MC_OP_HEARTBEAT             { 0x00, 0x08, 0x00, 0x01, 0x10, 0, 0, 0 }
#define MC_OP_CAP_RATE              { 0x07, 0x24, 0x00, 0x80, 0, 0, 0, 0 }
#define MC_OP_CAP_ENC               { 0x07, 0x24, 0x00, 0x82, 0, 0, 0, 0 }
// request opcode byte [3] (bytes 24..31 of the request: 07 ?? 00 XX 00 00 00 ??)
#define MC_REQ_BOARD_INFO           0x61u
#define MC_REQ_PRODUCT_INFO         0xc1u
#define MC_REQ_CLOCK_STATS          0x21u
#define MC_REQ_NETWORK_INFO         0x13u
#define MC_REQ_77                   0x77u
#define MC_REQ_CAP_RATE             0x81u
#define MC_REQ_CAP_ENC              0x83u
#define MC_REPLY_78_CONTENT         { 0, 0, 0, 3, 0, 0, 0, 0 }

#define MC_BOARD_INFO_LEN           200u
#define MC_PRODUCT_INFO_LEN         336u

// ---------------------------------------------------------------------------
// Flow control (protocol/flows_control.rs, flows_control_server.rs)
// ---------------------------------------------------------------------------
#define FC_START_CODE               0x1102u // "dbcp1"; Inferno forces this as client
#define FC_OP_REQUEST_FLOW          0x0100u
#define FC_OP_STOP_FLOW             0x0101u
#define FC_OP_UPDATE_FLOW           0x0102u
#define FC_ERR_FLOW_NOT_FOUND       0x0103u
#define FC_ERR_TOO_MANY_TX_FLOWS    0x0315u
#define FC_ERR_SAMPLE_RATE_MISMATCH 0x0301u
#define FC_ERR_0302                 0x0302u // Inferno: "0x0302u /* ??? TODO */"
#define FC_REMOTE_DESCR_0802        0x0802u
#define FC_REQ_NAMES_0A00           0x0a00u
#define FC_REQ_0002                 0x0002u
#define FC_REQ_1C                   0x1cu   // body.write_u16(0x1c + 2 * channels.len())
#define FC_REQ_STRINGS_BASE         0x26u   // strings_offset = 0x26 + n*2 + HEADER_LENGTH
#define FC_REPLY_TIMEOUT_MS         3000u
#define FC_HANDLE_LEN               6u

// ---------------------------------------------------------------------------
// Media (flows_tx.rs, flows_rx.rs)
// ---------------------------------------------------------------------------
#define MEDIA_HEADER_LEN            9u      // u8 2, u32 seconds, u32 subsec samples
#define MEDIA_BYTE0                 2u      // flows_tx.rs: pbuff[0] = 2u8; // ???
#define MEDIA_KEEPALIVE             { 0x13, 0x37 }
#define MEDIA_KEEPALIVE_INTERVAL_MS 250u
#define TX_KEEPALIVE_TIMEOUT_S      4u      // KEEPALIVE_TIMEOUT_SECONDS
#define TX_FPP_MIN                  2u      // FPP_MIN
#define TX_FPP_MAX                  256u    // FPP_MAX (accepted in requests)
// FPP_MAX_ADVERTISED (Inferno: 32, mDNS "fpp=32,2"). Ours follows the latency
// profile: a packet of fpp frames cannot arrive before fpp frames after its
// timestamp, so fpp must stay well inside the AoIP latency (app_config.h).
#define TX_FPP_MAX_ADVERTISED       UI_TX_FPP_MAX
#define TX_DISCONTINUITY_FRAMES     192000  // DISCONTINUITY_THRESHOLD_SAMPLES
#define RX_MAX_PAYLOAD_BYTES        1400u   // net.rs MAX_PAYLOAD_BYTES "// ???"
#define RX_FLOW_TIMEOUT_S           2u      // scan_flows: last packet older than 2 s
#define RX_FLOW_MIN_AGE_S           6u      // ... but only for flows older than 6 s
#define RX_RESOLVE_INTERVAL_S       9u      // retry unresolved subscriptions
#define RX_MAX_CH_PER_REQUESTED_FLOW 8u     // channels_subscriber.rs ".min(8 /*TODO*/)"
#define MCAST_GRACE_MS              1200u   // 3 x 400 ms A-record checks (mdns_client.rs)

// ---------------------------------------------------------------------------
// Device identity values copied from Inferno's settings.rs / mdns_server.rs
// ---------------------------------------------------------------------------
#define AOIP_VENDOR_STRING          "Audinate Dante-compatible" // truncated to 8 in mcast
#define AOIP_MODEL_NUMBER           "_000000000000000b"
#define AOIP_PCM_TYPE               0x0eu   // "usually 0xe, in older devices 4"
#define MDNS_ARC_TXT_ARCP_VERS      "arcp_vers=2.7.41"
#define MDNS_ARC_TXT_ARCP_MIN       "arcp_min=0.2.4"
#define MDNS_ARC_TXT_ROUTER_VERS    "router_vers=4.0.2"
#define MDNS_CMC_TXT_CMCP_VERS      "cmcp_vers=1.2.0"
#define MDNS_CMC_TXT_CMCP_MIN       "cmcp_min=1.0.0"
#define MDNS_CMC_TXT_SERVER_VERS    "server_vers=4.0.2"
#define MDNS_CMC_TXT_CHANNELS       "channels=0x6000004d"   // "// ???"
#define MDNS_CHAN_TXT_DBCP1         "0x1102"
#define MDNS_CHAN_TXT_DBCP          "0x1004"

// ---------------------------------------------------------------------------
// Our own choices (not protocol constants)
// ---------------------------------------------------------------------------
// Local UDP port of RX flow slot i (Inferno uses an ephemeral port per flow;
// a fixed range lets the EMAC fast path claim the frames).
#define AOIP_RX_PORT_BASE           14336u
// Local (source) UDP port of TX flow slot i. Receivers send keepalives here.
#define AOIP_TX_PORT_BASE           14400u
