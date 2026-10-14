// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB descriptors of the usb-inferno UAC2 function (High-Speed).
//
//   Device (IAD composite) -> Configuration 1
//     IAD: AUDIO / AF_VERSION_02_00, 3 interfaces
//     itf 0  AudioControl
//            Clock Source  0x09  internal fixed 48 kHz (freq + validity read-only)
//            IT 0x01 USB streaming  32 ch "Net TX 01..32" -> OT 0x02 digital audio itf
//            IT 0x03 digital audio itf 32 ch "Net RX 01..32" -> OT 0x04 USB streaming
//     itf 1  AudioStreaming OUT (alt 0: none; alt 1: 32 ch PCM 4/24, EP 0x01 + FB EP 0x81)
//     itf 2  AudioStreaming IN  (alt 0: none; alt 1: 32 ch PCM 4/24, EP 0x82)
//
// No Feature Units: the function is bit-transparent. At Full Speed the device
// presents an empty configuration (32 ch cannot fit in 12 Mbit/s).
#include "usb/uac2_desc.h"

#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "tusb.h"

#include "app_config.h"
#include "usb/uac2_defs.h"

// ---------------------------------------------------------------------------
// UAC2 constants (Audio Device Class 2.0, Appendix A)
// ---------------------------------------------------------------------------
#define CS_INTERFACE            0x24
#define CS_ENDPOINT             0x25
#define AUDIO_CLASS             0x01
#define AUDIO_SUBCLASS_CONTROL  0x01
#define AUDIO_SUBCLASS_STREAM   0x02
#define AUDIO_IP_V2             0x20
#define AUDIO_FUNC_SUBCLASS_UNDEF 0x00
#define AUDIO_AF_V2             0x20
#define AC_HEADER               0x01
#define AC_INPUT_TERMINAL       0x02
#define AC_OUTPUT_TERMINAL      0x03
#define AC_CLOCK_SOURCE         0x0A
#define AS_GENERAL              0x01
#define AS_FORMAT_TYPE          0x02
#define EP_GENERAL              0x01
#define CATEGORY_IO_BOX         0x08
#define TT_USB_STREAMING        0x0101
#define TT_DIGITAL_AUDIO_ITF    0x0602
#define FORMAT_TYPE_I           0x01
#define FORMAT_PCM              0x00000001u
#define CLK_ATTR_INTERNAL_FIXED 0x01
#define CLK_CTRL_FREQ_RO        0x01        // D1..0 = 01
#define CLK_CTRL_VALID_RO       0x04        // D3..2 = 01
// Endpoint bmAttributes
#define EP_ISO                  0x01
#define EP_SYNC_ASYNC           (0x01 << 2)
#define EP_USAGE_DATA           (0x00 << 4)
#define EP_USAGE_FEEDBACK       (0x01 << 4)

#define U16(x) (uint8_t)((x) & 0xFFu), (uint8_t)(((x) >> 8) & 0xFFu)
#define U32(x) U16((uint32_t)(x) & 0xFFFFu), U16(((uint32_t)(x) >> 16) & 0xFFFFu)

// Descriptor lengths
#define L_CFG       9
#define L_IAD       8
#define L_ITF       9
#define L_AC_HDR    9
#define L_CLK       8
#define L_IT        17
#define L_OT        12
#define L_AS_GEN    16
#define L_FMT1      6
#define L_EP        7
#define L_CS_EP     8

#define AC_CS_TOTAL   (L_AC_HDR + L_CLK + 2 * L_IT + 2 * L_OT)
#define AS_OUT_TOTAL  (2 * L_ITF + L_AS_GEN + L_FMT1 + L_EP + L_CS_EP + L_EP)
#define AS_IN_TOTAL   (2 * L_ITF + L_AS_GEN + L_FMT1 + L_EP + L_CS_EP)
#define CFG_HS_TOTAL  (L_CFG + L_IAD + L_ITF + AC_CS_TOTAL + AS_OUT_TOTAL + AS_IN_TOTAL)

#define ITF(num, alt, neps, sub) \
    L_ITF, TUSB_DESC_INTERFACE, (num), (alt), (neps), AUDIO_CLASS, (sub), AUDIO_IP_V2, 0

#define IT(id, type, nch, names) \
    L_IT, CS_INTERFACE, AC_INPUT_TERMINAL, (id), U16(type), 0x00, UAC2_ID_CLOCK, \
    (nch), U32(0), (names), U16(0), 0x00

#define OT(id, type, src) \
    L_OT, CS_INTERFACE, AC_OUTPUT_TERMINAL, (id), U16(type), 0x00, (src), UAC2_ID_CLOCK, U16(0), 0x00

#define AS_GEN(link, nch, names) \
    L_AS_GEN, CS_INTERFACE, AS_GENERAL, (link), 0x00, FORMAT_TYPE_I, U32(FORMAT_PCM), \
    (nch), U32(0), (names)

#define FMT1() \
    L_FMT1, CS_INTERFACE, AS_FORMAT_TYPE, FORMAT_TYPE_I, UI_USB_SUBSLOT, UI_USB_BITS

#define EP_DATA(addr) \
    L_EP, TUSB_DESC_ENDPOINT, (addr), (EP_ISO | EP_SYNC_ASYNC | EP_USAGE_DATA), \
    U16(UAC2_EP_DATA_MPS), UAC2_EP_DATA_BINTERVAL

#define CS_EP() \
    L_CS_EP, CS_ENDPOINT, EP_GENERAL, 0x00, 0x00, 0x00, U16(0)

#define EP_FB(addr) \
    L_EP, TUSB_DESC_ENDPOINT, (addr), (EP_ISO | EP_USAGE_FEEDBACK), \
    U16(UAC2_EP_FB_MPS), UAC2_EP_FB_BINTERVAL

#define CONFIG_HS(desc_type) {                                                       \
    /* Configuration */                                                              \
    L_CFG, (desc_type), U16(CFG_HS_TOTAL), UAC2_ITF_COUNT, 1, 0,                     \
    TU_BIT(7), (UAC2_MAX_POWER_MA / 2),                                              \
    /* Interface Association */                                                      \
    L_IAD, TUSB_DESC_INTERFACE_ASSOCIATION, UAC2_ITF_AC, UAC2_ITF_COUNT,             \
    AUDIO_CLASS, AUDIO_FUNC_SUBCLASS_UNDEF, AUDIO_AF_V2, UAC2_STR_PRODUCT,           \
    /* AudioControl */                                                               \
    ITF(UAC2_ITF_AC, 0, 0, AUDIO_SUBCLASS_CONTROL),                                  \
    L_AC_HDR, CS_INTERFACE, AC_HEADER, U16(0x0200), CATEGORY_IO_BOX,                 \
    U16(AC_CS_TOTAL), 0x00,                                                          \
    L_CLK, CS_INTERFACE, AC_CLOCK_SOURCE, UAC2_ID_CLOCK, CLK_ATTR_INTERNAL_FIXED,    \
    (CLK_CTRL_FREQ_RO | CLK_CTRL_VALID_RO), 0x00, 0x00,                              \
    IT(UAC2_ID_IT_USB_OUT, TT_USB_STREAMING, UI_CHANNELS, UAC2_STR_CH_OUT_FIRST),    \
    OT(UAC2_ID_OT_NET_TX, TT_DIGITAL_AUDIO_ITF, UAC2_ID_IT_USB_OUT),                 \
    IT(UAC2_ID_IT_NET_RX, TT_DIGITAL_AUDIO_ITF, UI_CHANNELS, UAC2_STR_CH_IN_FIRST),  \
    OT(UAC2_ID_OT_USB_IN, TT_USB_STREAMING, UAC2_ID_IT_NET_RX),                      \
    /* AudioStreaming OUT (host playback) */                                         \
    ITF(UAC2_ITF_OUT, 0, 0, AUDIO_SUBCLASS_STREAM),                                  \
    ITF(UAC2_ITF_OUT, 1, 2, AUDIO_SUBCLASS_STREAM),                                  \
    AS_GEN(UAC2_ID_IT_USB_OUT, UI_CHANNELS, UAC2_STR_CH_OUT_FIRST),                  \
    FMT1(),                                                                          \
    EP_DATA(UAC2_EP_OUT),                                                            \
    CS_EP(),                                                                         \
    EP_FB(UAC2_EP_FB),                                                               \
    /* AudioStreaming IN (host capture) */                                           \
    ITF(UAC2_ITF_IN, 0, 0, AUDIO_SUBCLASS_STREAM),                                   \
    ITF(UAC2_ITF_IN, 1, 1, AUDIO_SUBCLASS_STREAM),                                   \
    AS_GEN(UAC2_ID_OT_USB_IN, UI_CHANNELS, UAC2_STR_CH_IN_FIRST),                    \
    FMT1(),                                                                          \
    EP_DATA(UAC2_EP_IN),                                                             \
    CS_EP(),                                                                         \
}

static const uint8_t s_cfg_hs[] = CONFIG_HS(TUSB_DESC_CONFIGURATION);
static const uint8_t s_cfg_hs_other[] = CONFIG_HS(TUSB_DESC_OTHER_SPEED_CONFIG);
_Static_assert(sizeof s_cfg_hs == CFG_HS_TOTAL, "HS configuration length");

// Full Speed: a valid configuration without interfaces.
#define CONFIG_FS(desc_type) { \
    L_CFG, (desc_type), U16(L_CFG), 0, 1, 0, TU_BIT(7), (100 / 2) }
static const uint8_t s_cfg_fs[] = CONFIG_FS(TUSB_DESC_CONFIGURATION);
static const uint8_t s_cfg_fs_other[] = CONFIG_FS(TUSB_DESC_OTHER_SPEED_CONFIG);

static const tusb_desc_device_t s_dev = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = UAC2_USB_VID,
    .idProduct          = UAC2_USB_PID,
    .bcdDevice          = UAC2_USB_BCD_DEVICE,
    .iManufacturer      = UAC2_STR_MANUFACTURER,
    .iProduct           = UAC2_STR_PRODUCT,
    .iSerialNumber      = UAC2_STR_SERIAL,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_qual = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved          = 0,
};

static char s_serial[13];       // Ethernet MAC, 12 hex digits

void uac2_desc_init(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_ETH) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_BASE);
    }
    snprintf(s_serial, sizeof s_serial, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ---------------------------------------------------------------------------
// TinyUSB descriptor callbacks (task context)
// ---------------------------------------------------------------------------
const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_dev;
}

const uint8_t *tud_descriptor_device_qualifier_cb(void)
{
    return (const uint8_t *)&s_qual;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return tud_speed_get() == TUSB_SPEED_HIGH ? s_cfg_hs : s_cfg_fs;
}

const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    return tud_speed_get() == TUSB_SPEED_HIGH ? s_cfg_fs_other : s_cfg_hs_other;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[1 + 32];
    char tmp[32];
    const char *s;

    if (index == UAC2_STR_LANGID) {
        buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        buf[1] = 0x0409;
        return buf;
    }
    if (index == UAC2_STR_MANUFACTURER) {
        s = UI_MANUFACTURER;
    } else if (index == UAC2_STR_PRODUCT) {
        s = UI_MODEL_NAME;
    } else if (index == UAC2_STR_SERIAL) {
        s = s_serial;
    } else if (index >= UAC2_STR_CH_OUT_FIRST && index < UAC2_STR_CH_IN_FIRST) {
        snprintf(tmp, sizeof tmp, "Net TX %02u", (unsigned)(index - UAC2_STR_CH_OUT_FIRST + 1));
        s = tmp;
    } else if (index >= UAC2_STR_CH_IN_FIRST && index < UAC2_STR_COUNT) {
        snprintf(tmp, sizeof tmp, "Net RX %02u", (unsigned)(index - UAC2_STR_CH_IN_FIRST + 1));
        s = tmp;
    } else {
        return NULL;
    }

    size_t n = strlen(s);
    if (n > 31) n = 31;
    for (size_t i = 0; i < n; i++) {
        buf[1 + i] = (uint8_t)s[i];
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}
