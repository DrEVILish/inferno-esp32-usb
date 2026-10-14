// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB Audio Class 2.0 on the ESP32-P4 HS OTG port: TinyUSB glue.
//
// TinyUSB (espressif/tinyusb, DWC2 Buffer-DMA) provides enumeration and
// endpoint transfers. The audio function is our own small class driver
// (usbd_app_driver_get_cb): TinyUSB's audio class moves samples through
// software FIFOs and sizes IN packets from FIFO fill, which costs copies and
// cannot give "exactly the media-clock advance per microframe". Here every
// data-path event runs in the USB interrupt:
//
//   SOF        (usbd_class_driver_t.sof, ISR)  latch mc_now_ns() -> engine;
//                                              arm streams that just started
//   OUT done   (.xfer_isr)  re-arm the other buffer, then copy to the TX ring
//   IN done    (.xfer_isr)  build the next packet from the RX ring, arm it
//   FB done    (.xfer_isr)  arm the next feedback value
//
// Control requests (SET_INTERFACE, clock source GET CUR/RANGE) run in the
// TinyUSB task, pinned to UI_CORE_USB. tusb_init() is called from that task,
// so the USB interrupt is allocated on the same core: a critical section in
// the task therefore excludes the ISR. The clock/position logic itself lives
// in uac2_engine.c (pure C, host-testable).
#include "usb/uac2.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "esp_intr_alloc.h"
#include "soc/interrupts.h"

#include "tusb.h"
#include "device/usbd_pvt.h"

#include "app_config.h"
#include "clock/mediaclock.h"
#include "usb/uac2_defs.h"
#include "usb/uac2_desc.h"
#include "usb/uac2_engine.h"

static const char *TAG = "uac2";

#define RHPORT          1u      // TinyUSB DWC2 on ESP32-P4: port 0 = OTG FS, port 1 = OTG HS
#define BUF_ALIGN       CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define ARM_DELAY_SOF   2u      // SOFs between an alt change and the first transfer

// DMA buffers are whole cache lines (TinyUSB cleans/invalidates rounded up to
// the line size); at low channel counts the packet itself is smaller.
#define DMA_BUF_BYTES   ((UAC2_EP_DATA_MPS + BUF_ALIGN - 1u) / BUF_ALIGN * BUF_ALIGN)

// UAC2 request codes / control selectors
#define UAC2_REQ_CUR            0x01
#define UAC2_REQ_RANGE          0x02
#define UAC2_CS_SAM_FREQ        0x01
#define UAC2_CS_CLOCK_VALID     0x02

// ---------------------------------------------------------------------------
// State. Data-path state is touched only in the USB ISR or under s_lock.
// DMA buffers: internal RAM (.bss), cache-line aligned and sized.
// ---------------------------------------------------------------------------
static uac2_engine_t s_eng;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t s_out_buf[2][DMA_BUF_BYTES] __attribute__((aligned(BUF_ALIGN)));
static uint8_t s_in_buf[2][DMA_BUF_BYTES] __attribute__((aligned(BUF_ALIGN)));
static uint32_t s_fb_buf[BUF_ALIGN / sizeof(uint32_t)] __attribute__((aligned(BUF_ALIGN)));

static const tusb_desc_endpoint_t *s_ep_out, *s_ep_fb, *s_ep_in;
static volatile uint8_t s_alt_out, s_alt_in;
static bool s_out_armed, s_fb_armed, s_in_armed;
static uint32_t s_out_arm_at, s_in_arm_at;  // engine sof_count from which arming is allowed
static uint8_t s_out_idx, s_in_idx;
static uint64_t s_out_arm_uf[2];            // engine microframe each OUT buffer was armed in
static bool s_configured;

static uint32_t s_out_xfer_err, s_in_xfer_err, s_fb_xfer_err, s_arm_fail;
// IN read age window (reset by uac2_get_stats): real media time when an IN
// packet is built minus the newest frame it carries, in frames. The device's
// network->USB residence for that sample (UI_NET_LATENCY + IN margin + up to
// one microframe); the host receives it within the next microframe.
static int32_t s_in_age_min = INT32_MAX, s_in_age_max = INT32_MIN;

static usb_phy_handle_t s_phy;

// ---------------------------------------------------------------------------
// Arming (ISR context, s_lock held)
// ---------------------------------------------------------------------------
static void IRAM_ATTR arm_out(void)
{
    s_out_arm_uf[s_out_idx] = eng_uf(&s_eng);
    s_out_armed = usbd_edpt_xfer(RHPORT, UAC2_EP_OUT, s_out_buf[s_out_idx], UAC2_EP_DATA_MPS, true);
    if (!s_out_armed) s_arm_fail++;
}

static void IRAM_ATTR arm_fb(void)
{
    s_fb_buf[0] = eng_fb_q16(&s_eng);           // 16.16, little-endian on the wire
    s_fb_armed = usbd_edpt_xfer(RHPORT, UAC2_EP_FB, (uint8_t *)s_fb_buf, UAC2_EP_FB_MPS, true);
    if (!s_fb_armed) s_arm_fail++;
}

static void IRAM_ATTR arm_in(void)
{
    s_in_idx ^= 1u;
    uint32_t len = eng_in_packet(&s_eng, s_in_buf[s_in_idx]);
    if (len) {
        const int32_t age = (int32_t)(int64_t)(mc_ns_to_frames(mc_now_ns()) - (s_eng.r - 1u));
        if (age < s_in_age_min) s_in_age_min = age;
        if (age > s_in_age_max) s_in_age_max = age;
    }
    s_in_armed = usbd_edpt_xfer(RHPORT, UAC2_EP_IN, s_in_buf[s_in_idx], (uint16_t)len, true);
    if (!s_in_armed) s_arm_fail++;
}

// ---------------------------------------------------------------------------
// Class driver: ISR-context callbacks
// ---------------------------------------------------------------------------
static void IRAM_ATTR uac2_sof_isr(uint8_t rhport, uint32_t frame_count)
{
    (void)rhport;
    uint32_t epoch;
    const uint64_t t = mc_now_ns_epoch(&epoch); // latch first: closest to the SOF

    portENTER_CRITICAL_ISR(&s_lock);
    eng_sof(&s_eng, t, epoch, frame_count);
    const uint32_t n = s_eng.st.sof_count;
    if (s_alt_out && (int32_t)(n - s_out_arm_at) >= 0) {
        if (!s_out_armed) arm_out();
        if (!s_fb_armed) arm_fb();
    }
    if (s_alt_in && !s_in_armed && (int32_t)(n - s_in_arm_at) >= 0) {
        arm_in();
    }
    portEXIT_CRITICAL_ISR(&s_lock);
}

static bool IRAM_ATTR uac2_xfer_isr(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t len)
{
    (void)rhport;
    portENTER_CRITICAL_ISR(&s_lock);
    switch (ep_addr) {
    case UAC2_EP_OUT:
        if (s_out_armed) {
            const uint8_t di = s_out_idx;
            uint8_t *done = s_out_buf[di];
            const uint64_t arm_uf = s_out_arm_uf[di];
            s_out_idx ^= 1u;
            s_out_armed = false;
            if (s_alt_out) arm_out();           // next microframe first, then copy
            if (result == XFER_RESULT_SUCCESS) {
                eng_out_packet(&s_eng, mc_ns_to_frames(mc_now_ns()), arm_uf, done, len);
            } else {
                s_out_xfer_err++;
            }
        }
        break;
    case UAC2_EP_FB:
        if (s_fb_armed) {
            s_fb_armed = false;
            if (result != XFER_RESULT_SUCCESS) s_fb_xfer_err++;
            if (s_alt_out) arm_fb();
        }
        break;
    case UAC2_EP_IN:
        if (s_in_armed) {
            s_in_armed = false;
            // FAILED = the host did not collect it in its microframe (the DWC2
            // driver retried once). Those frames are lost; r stays continuous.
            if (result != XFER_RESULT_SUCCESS) s_in_xfer_err++;
            if (s_alt_in) arm_in();
        }
        break;
    default:
        break;
    }
    portEXIT_CRITICAL_ISR(&s_lock);
    return true;                                // never defer to the task
}

static bool uac2_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t len)
{
    (void)rhport; (void)ep_addr; (void)result; (void)len;
    return true;                                // all handled in uac2_xfer_isr
}

// ---------------------------------------------------------------------------
// Class driver: task-context callbacks
// ---------------------------------------------------------------------------
static void uac2_init(void) {}

static bool uac2_deinit(void) { return true; }

static void uac2_reset(uint8_t rhport)
{
    (void)rhport;
    portENTER_CRITICAL(&s_lock);
    s_alt_out = s_alt_in = 0;
    s_out_armed = s_fb_armed = s_in_armed = false;
    s_configured = false;
    s_ep_out = s_ep_fb = s_ep_in = NULL;
    eng_bus_reset(&s_eng);
    portEXIT_CRITICAL(&s_lock);
}

static uint16_t uac2_open(uint8_t rhport, const tusb_desc_interface_t *itf, uint16_t max_len)
{
    if (itf->bInterfaceClass != TUSB_CLASS_AUDIO || itf->bInterfaceSubClass != 0x01 ||
        itf->bInterfaceNumber != UAC2_ITF_AC || itf->bAlternateSetting != 0) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)itf;
    const uint8_t *end = p + max_len;
    const tusb_desc_endpoint_t *out = NULL, *fb = NULL, *in = NULL;

    // Consume the AC interface and the two AS interfaces that follow it.
    while (end - p >= 2 && p[0] >= 2 && end - p >= p[0]) {
        const uint8_t type = p[1];
        if (type == TUSB_DESC_INTERFACE_ASSOCIATION) break;
        if (type == TUSB_DESC_INTERFACE &&
            ((const tusb_desc_interface_t *)p)->bInterfaceNumber >= UAC2_ITF_AC + UAC2_ITF_COUNT) break;
        if (type == TUSB_DESC_ENDPOINT) {
            const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p;
            if (ep->bEndpointAddress == UAC2_EP_OUT) out = ep;
            else if (ep->bEndpointAddress == UAC2_EP_FB) fb = ep;
            else if (ep->bEndpointAddress == UAC2_EP_IN) in = ep;
            // Reserve the FIFO for the largest packet now; activated on
            // SET_INTERFACE. The 4-byte feedback EP gets 16 words, the DWC2
            // minimum TX FIFO depth (TinyUSB would allocate just 1 word).
            uint16_t fifo = tu_edpt_packet_size(ep);
            if (fifo < 64) fifo = 64;
            TU_ASSERT(usbd_edpt_iso_alloc(rhport, ep->bEndpointAddress, fifo), 0);
        }
        p += p[0];
    }
    TU_VERIFY(out && fb && in, 0);

    portENTER_CRITICAL(&s_lock);
    s_ep_out = out;
    s_ep_fb = fb;
    s_ep_in = in;
    s_configured = true;
    portEXIT_CRITICAL(&s_lock);

    usbd_sof_enable(rhport, SOF_CONSUMER_AUDIO, true);
    return (uint16_t)(p - (const uint8_t *)itf);
}

// Called with the lock NOT held. Runs entirely inside a critical section on
// the USB core: the endpoint (re)activation polls DWC2 interrupt bits that
// the ISR would otherwise clear underneath it.
static bool set_interface(uint8_t rhport, uint8_t itf, uint8_t alt)
{
    if (itf == UAC2_ITF_AC) return alt == 0;
    if ((itf != UAC2_ITF_OUT && itf != UAC2_ITF_IN) || alt > 1) return false;
    if (!s_configured) return false;

    bool ok = true;
    portENTER_CRITICAL(&s_lock);
    const uint32_t arm_at = s_eng.st.sof_count + ARM_DELAY_SOF;
    if (itf == UAC2_ITF_OUT) {
        s_alt_out = 0;
        s_out_armed = s_fb_armed = false;       // forget anything in flight
        // (Re)activating disables the endpoint first, which aborts a pending
        // transfer; it also leaves alt 0 with idle, unarmed endpoints.
        ok = usbd_edpt_iso_activate(rhport, s_ep_out) && usbd_edpt_iso_activate(rhport, s_ep_fb);
        s_out_idx = 0;
        s_out_arm_at = arm_at;
        eng_out_alt(&s_eng, ok && alt == 1);
        s_alt_out = (ok && alt == 1) ? 1 : 0;
    } else {
        s_alt_in = 0;
        s_in_armed = false;
        ok = usbd_edpt_iso_activate(rhport, s_ep_in);
        s_in_arm_at = arm_at;
        eng_in_alt(&s_eng, ok && alt == 1);
        s_alt_in = (ok && alt == 1) ? 1 : 0;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

static bool clock_request(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req)
{
    static uint8_t buf[16];
    const uint8_t cs = tu_u16_high(req->wValue);
    const bool get = req->bmRequestType_bit.direction == TUSB_DIR_IN;

    if (get) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        if (cs == UAC2_CS_SAM_FREQ && req->bRequest == UAC2_REQ_CUR) {
            const uint32_t f = UI_SAMPLE_RATE;
            memcpy(buf, &f, 4);
            return tud_control_xfer(rhport, req, buf, 4);
        }
        if (cs == UAC2_CS_SAM_FREQ && req->bRequest == UAC2_REQ_RANGE) {
            // wNumSubRanges = 1, { dMIN, dMAX, dRES } = { 48000, 48000, 0 }
            const uint16_t nsub = 1;
            const uint32_t r[3] = { UI_SAMPLE_RATE, UI_SAMPLE_RATE, 0 };
            memcpy(buf, &nsub, 2);
            memcpy(buf + 2, r, sizeof r);
            return tud_control_xfer(rhport, req, buf, 2 + sizeof r);
        }
        if (cs == UAC2_CS_CLOCK_VALID && req->bRequest == UAC2_REQ_CUR) {
            buf[0] = 1;
            return tud_control_xfer(rhport, req, buf, 1);
        }
        return false;
    }

    // SET CUR sampling frequency: the control is read-only, but accept a
    // host that writes the one rate we have (some do regardless).
    if (cs != UAC2_CS_SAM_FREQ || req->bRequest != UAC2_REQ_CUR || req->wLength != 4) return false;
    if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, req, buf, 4);
    if (stage == CONTROL_STAGE_DATA) {
        uint32_t f;
        memcpy(&f, buf, 4);
        return f == UI_SAMPLE_RATE;
    }
    return true;
}

static bool uac2_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req)
{
    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) {
        return false;                           // no endpoint controls
    }
    const uint8_t itf = tu_u16_low(req->wIndex);

    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        if (req->bRequest == TUSB_REQ_SET_INTERFACE) {
            if (!set_interface(rhport, itf, (uint8_t)req->wValue)) return false;
            return tud_control_status(rhport, req);
        }
        if (req->bRequest == TUSB_REQ_GET_INTERFACE) {
            static uint8_t alt;
            alt = itf == UAC2_ITF_OUT ? s_alt_out : itf == UAC2_ITF_IN ? s_alt_in : 0;
            return tud_control_xfer(rhport, req, &alt, 1);
        }
        return false;
    }

    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS && itf == UAC2_ITF_AC &&
        tu_u16_high(req->wIndex) == UAC2_ID_CLOCK) {
        return clock_request(rhport, stage, req);
    }
    return false;                               // terminals/AS: no controls
}

static const usbd_class_driver_t s_driver = {
    .name = "UAC2-INF",
    .init = uac2_init,
    .deinit = uac2_deinit,
    .reset = uac2_reset,
    .open = uac2_open,
    .control_xfer_cb = uac2_control_xfer_cb,
    .xfer_cb = uac2_xfer_cb,
    .xfer_isr = uac2_xfer_isr,
    .sof = uac2_sof_isr,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_driver;
}

// ---------------------------------------------------------------------------
// IRAM-safe USB interrupt
// ---------------------------------------------------------------------------
// TinyUSB's dwc2_esp32.h allocates the DWC2 interrupt with
// ESP_INTR_FLAG_LOWMED only, so it is masked for the whole duration of every
// SPI-flash write or erase (NVS commit): tens of ms without SOF/IN/OUT
// service. The link step wraps esp_intr_alloc() (main/CMakeLists.txt,
// -Wl,--wrap=esp_intr_alloc); for the USB OTG HS source we add
// ESP_INTR_FLAG_IRAM. Everything the handler reaches is in IRAM/DRAM
// (main/linker.lf + IRAM_ATTR), checked after each build by
// tools/check_iram.py.
esp_err_t __real_esp_intr_alloc(int source, int flags, intr_handler_t handler, void *arg,
                                intr_handle_t *ret_handle);
static bool s_usb_intr_iram;

esp_err_t __wrap_esp_intr_alloc(int source, int flags, intr_handler_t handler, void *arg,
                                intr_handle_t *ret_handle)
{
    if (source == ETS_USB_OTG_INTR_SOURCE) {
        flags |= ESP_INTR_FLAG_IRAM;
        s_usb_intr_iram = true;
    }
    return __real_esp_intr_alloc(source, flags, handler, arg, ret_handle);
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
static void usb_task(void *arg)
{
    TaskHandle_t parent = (TaskHandle_t)arg;
    const tusb_rhport_init_t init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_HIGH,
    };
    // Initialising here puts the DWC2 interrupt on this task's core.
    const bool ok = tusb_rhport_init(RHPORT, &init);
    xTaskNotify(parent, ok ? 1u : 2u, eSetValueWithOverwrite);
    if (!ok) {
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        tud_task();
    }
}

esp_err_t uac2_start(void)
{
    static bool started;
    if (started) return ESP_OK;

    eng_init(&s_eng);
    uac2_desc_init();

    const usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_UTMI,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_HIGH,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,
    };
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy, &s_phy), TAG, "UTMI PHY");

    xTaskNotifyStateClear(NULL);
    if (xTaskCreatePinnedToCore(usb_task, "usb", 4096, xTaskGetCurrentTaskHandle(),
                                UI_PRIO_USB, NULL, UI_CORE_USB) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t v = 0;
    if (xTaskNotifyWait(0, UINT32_MAX, &v, pdMS_TO_TICKS(5000)) != pdTRUE || v != 1u) {
        ESP_LOGE(TAG, "TinyUSB init failed");
        return ESP_FAIL;
    }
    started = true;
    if (!s_usb_intr_iram) {
        // The wrap did not apply (link flags lost?): USB still works, but
        // every flash write stalls it. tools/check_iram.py cannot see this.
        ESP_LOGE(TAG, "DWC2 interrupt is NOT IRAM-safe (esp_intr_alloc wrap missing)");
    }
    ESP_LOGI(TAG, "UAC2 %ux%u @ %u Hz on HS OTG (EP %u B, fb every %u uframes)",
             UI_CHANNELS, UI_CHANNELS, UI_SAMPLE_RATE, UAC2_EP_DATA_MPS,
             1u << (UAC2_EP_FB_BINTERVAL - 1u));
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------
void uac2_get_stats(uac2_stats_t *s)
{
    uac2_eng_stats_t st;
    uint32_t fb_q24;
    int32_t lead, ppb;
    bool out_active, in_streaming;
    uint8_t alt_out, alt_in, cfg;
    uint32_t oerr, ierr, ferr, afail;

    portENTER_CRITICAL(&s_lock);
    st = s_eng.st;
    fb_q24 = s_eng.fb_q24;
    lead = eng_out_lead(&s_eng);
    ppb = s_eng.phase_ppb;
    out_active = eng_out_active(&s_eng);
    in_streaming = s_eng.in_streaming;
    alt_out = s_alt_out;
    alt_in = s_alt_in;
    cfg = s_configured;
    oerr = s_out_xfer_err;
    ierr = s_in_xfer_err;
    ferr = s_fb_xfer_err;
    afail = s_arm_fail;
    const int32_t age_min = s_in_age_min, age_max = s_in_age_max;
    s_in_age_min = INT32_MAX;
    s_in_age_max = INT32_MIN;
    eng_latency_window_reset(&s_eng);
    portEXIT_CRITICAL(&s_lock);

    memset(s, 0, sizeof *s);
    s->sof_count = st.sof_count;
    s->out_frames = st.out_frames;
    s->in_frames = st.in_frames;
    s->out_overruns = st.out_overruns;
    s->out_underruns = st.out_underruns;
    s->in_underruns = st.in_underruns + ierr;
    s->feedback_q16 = fb_q24 >> 8;
    s->out_lead_frames = lead;
    s->out_streaming = out_active;
    s->in_streaming = alt_in && in_streaming;

    s->sof_missed = st.sof_missed;
    s->sof_late = st.sof_late;
    s->sof_gaps = st.sof_gaps;
    s->sof_fn_bad = st.sof_fn_bad;
    s->epoch_changes = st.epoch_changes;
    s->out_packets = st.out_packets;
    s->in_packets = st.in_packets;
    s->out_gap_fills = st.out_gap_fills;
    s->out_gap_frames = st.out_gap_frames;
    s->out_bad_len = st.out_bad_len;
    s->in_catchup = st.in_catchup;
    s->in_resyncs = st.in_resyncs;
    s->xfer_errors = oerr + ierr + ferr;
    s->arm_failures = afail;
    s->fb_phase_ppb = ppb;
    s->out_hr_min = st.out_hr_min;
    s->out_hr_max = st.out_hr_max;
    s->in_age_min = age_min;
    s->in_age_max = age_max;
    s->configured = cfg;
    s->high_speed = tud_speed_get() == TUSB_SPEED_HIGH;
    s->out_alt = alt_out;
    s->in_alt = alt_in;
}
