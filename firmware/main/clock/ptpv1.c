// SPDX-License-Identifier: GPL-3.0-or-later
//
// PTPv1 (IEEE 1588-2002) follower over UDP/IPv4, as used by AoIP networks:
// multicast 224.0.1.129, event port 319, general port 320, subdomain "_DFLT".
//
//   Sync        319  leader -> all   HW RX timestamp t2 (EMAC classifier)
//   Follow_Up   320  leader -> all   precise origin t1 of that Sync
//   Delay_Req   319  us -> leader    HW TX timestamp t3
//   Delay_Resp  320  leader -> all   receipt time t4, addressed by our UUID
//
// Frames arrive through net/ hooks in the EMAC RX task, are parsed there into
// a small event and queued to the PTP task (core 0), which runs leader
// selection, the servo (ptp_servo.c) and Delay_Req. We never send Sync.
#include "clock/ptpv1.h"
#include "clock/mc_hw.h"
#include "clock/ptp_servo.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "app_config.h"
#include "net/net.h"
#include "net/net_priv.h"

static const char *TAG = "ptpv1";

// ---- protocol constants (IEEE 1588-2002) -----------------------------------
#define PTP_EVENT_PORT          319
#define PTP_GENERAL_PORT        320
#define PTP_HDR_LEN             40
#define PTP_SYNC_LEN            124     // Sync and Delay_Req
#define PTP_FUP_LEN             52
#define PTP_DRESP_LEN           60
#define PTP_CTL_SYNC            0
#define PTP_CTL_DELAY_REQ       1
#define PTP_CTL_FOLLOW_UP       2
#define PTP_CTL_DELAY_RESP      3
#define PTP_MSG_EVENT           1
#define PTP_MSG_GENERAL         2
#define PTP_COMM_ETHER          1
#define PTP_FLAG_ASSIST         0x0008u // two-step: a Follow_Up carries the precise origin
#define PTP_OUR_PORT_ID         1
static const uint8_t k_subdomain[16] = { '_', 'D', 'F', 'L', 'T' };
static const uint8_t k_group[4] = { 224, 0, 1, 129 };

// ---- behaviour -------------------------------------------------------------
#define MAX_CANDIDATES          4
#define LEADER_TIMEOUT_MS       3000    // no Sync from the leader for this long: lost
#define CANDIDATE_TIMEOUT_MS    3000
#define CANDIDATE_MIN_SYNCS     2       // a new leader must have been heard twice
#define DREQ_FAST_MS            250
#define DREQ_SLOW_MS            1000    // ± 25 % jitter
#define DREQ_TIMEOUT_MS         1500
#define EVT_QUEUE_LEN           16
#define PTP_TASK_STACK          4096
#define LOG_MIN_INTERVAL_MS     1000

// ---- events from the RX hook to the task -----------------------------------
typedef enum { EV_SYNC, EV_FUP, EV_DRESP } ev_kind_t;

typedef struct {
    uint8_t  uuid[6];
    uint16_t port;
} port_id_t;

typedef struct {
    port_id_t gm;
    uint8_t   stratum;
    uint8_t   ident[4];
    int16_t   variance;
    uint8_t   preferred;
    uint8_t   boundary;
    int8_t    sync_interval;
    uint16_t  steps_removed;
} gm_info_t;

typedef struct {
    uint8_t   kind;
    uint8_t   two_step;
    uint16_t  seq;
    port_id_t src;
    int64_t   ts;           // origin / precise origin / receipt time (leader), ns
    uint64_t  rx_ts;        // Sync: our HW RX timestamp, ns (0 = none)
    uint32_t  epoch;        // mc_epoch() when the hook ran: rx_ts's timescale
    union {
        gm_info_t gm;       // Sync
        struct {            // Delay_Resp
            port_id_t req;
            uint16_t  req_seq;
        } dr;
    };
} ptp_evt_t;

// ---- state -----------------------------------------------------------------
typedef struct {
    bool      used;
    port_id_t src;
    gm_info_t gm;
    uint32_t  last_ms;
    uint32_t  syncs;
} cand_t;

static QueueHandle_t s_q;
static servo_t       s_servo;
static uint8_t       s_uuid[6];
static net_udp_dest_t s_dst_event;

static cand_t        s_cand[MAX_CANDIDATES];
static int           s_leader = -1;             // index into s_cand
static port_id_t     s_leader_id;               // published copy
static volatile bool s_have_leader;

static struct { bool valid; uint16_t seq; uint64_t t2; } s_pending;        // two-step Sync
static struct { bool valid; uint16_t seq; uint64_t t3; uint32_t sent_ms; } s_dreq;
static uint16_t      s_dreq_seq;
static uint32_t      s_next_dreq_ms;

static volatile bool    s_locked;
static volatile int32_t s_offset_ns;
static volatile int32_t s_delay_ns;
static volatile uint8_t s_stage;
static volatile uint32_t s_syncs, s_fups, s_dreqs, s_dresps, s_no_rx_ts, s_no_tx_ts, s_leader_changes;
static _Atomic uint32_t  s_qdrops;
static uint32_t          s_last_log_ms;

// ---- helpers ---------------------------------------------------------------
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline int64_t ptp_ts(const uint8_t *p)
{
    // TimeRepresentation: UInteger32 seconds, Integer32 nanoseconds
    return (int64_t)rd32(p) * 1000000000LL + (int32_t)rd32(p + 4);
}
static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline bool port_eq(const port_id_t *a, const port_id_t *b)
{
    return a->port == b->port && memcmp(a->uuid, b->uuid, 6) == 0;
}

static bool log_ok(void)
{
    uint32_t t = now_ms();
    if (t - s_last_log_ms < LOG_MIN_INTERVAL_MS) {
        return false;
    }
    s_last_log_ms = t;
    return true;
}

// ---- RX hook (EMAC RX task): parse, queue, never block ----------------------
static bool ptp_hook(const uint8_t *p, size_t len, const net_udp_meta_t *m, void *ctx)
{
    (void)ctx;
    // versionPTP = 1, versionNetwork = 1 (a PTPv2 frame has 2 in byte 1)
    if (len < PTP_HDR_LEN || rd16(p) != 1 || memcmp(p + 4, k_subdomain, sizeof k_subdomain) != 0) {
        return true;    // nobody else on this device listens on 319/320
    }
    ptp_evt_t ev;
    memcpy(ev.src.uuid, p + 22, 6);
    ev.src.port = rd16(p + 28);
    ev.seq = rd16(p + 30);
    ev.rx_ts = 0;
    ev.two_step = 0;
    ev.epoch = mc_epoch();
    uint8_t ctl = p[32];

    if (m->dst_port == PTP_EVENT_PORT && ctl == PTP_CTL_SYNC && len >= PTP_SYNC_LEN) {
        ev.kind = EV_SYNC;
        ev.two_step = (rd16(p + 34) & PTP_FLAG_ASSIST) != 0;
        ev.ts = ptp_ts(p + 40);
        ev.rx_ts = m->rx_ts_ns;
        memcpy(ev.gm.gm.uuid, p + 54, 6);
        ev.gm.gm.port = rd16(p + 60);
        ev.gm.stratum = p[67];
        memcpy(ev.gm.ident, p + 68, 4);
        ev.gm.variance = (int16_t)rd16(p + 74);
        ev.gm.preferred = p[77];
        ev.gm.boundary = p[79];
        ev.gm.sync_interval = (int8_t)p[83];
        ev.gm.steps_removed = rd16(p + 90);
    } else if (m->dst_port == PTP_GENERAL_PORT && ctl == PTP_CTL_FOLLOW_UP && len >= PTP_FUP_LEN) {
        ev.kind = EV_FUP;
        ev.seq = rd16(p + 42);          // associatedSequenceId
        ev.ts = ptp_ts(p + 44);
    } else if (m->dst_port == PTP_GENERAL_PORT && ctl == PTP_CTL_DELAY_RESP && len >= PTP_DRESP_LEN) {
        ev.kind = EV_DRESP;
        ev.ts = ptp_ts(p + 40);
        memcpy(ev.dr.req.uuid, p + 50, 6);
        ev.dr.req.port = rd16(p + 56);
        ev.dr.req_seq = rd16(p + 58);
        if (ev.dr.req.port != PTP_OUR_PORT_ID || memcmp(ev.dr.req.uuid, s_uuid, 6) != 0) {
            return true;                // somebody else's
        }
    } else {
        return true;                    // Delay_Req of other followers, management, ...
    }
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&s_qdrops, 1, memory_order_relaxed);
    }
    return true;
}

// ---- best master (IEEE 1588-2002 data set comparison, simplified) ----------
// < 0 if a is the better leader.
static int bmc_cmp(const cand_t *a, const cand_t *b)
{
    if (port_eq(&a->gm.gm, &b->gm.gm)) {
        // Same grandmaster: nearer wins, then the lower sender identity.
        if (a->gm.steps_removed != b->gm.steps_removed) {
            return a->gm.steps_removed < b->gm.steps_removed ? -1 : 1;
        }
        int c = memcmp(a->src.uuid, b->src.uuid, 6);
        return c ? c : (int)a->src.port - (int)b->src.port;
    }
    if (a->gm.preferred != b->gm.preferred) {
        return a->gm.preferred ? -1 : 1;
    }
    if (a->gm.stratum != b->gm.stratum) {
        return a->gm.stratum < b->gm.stratum ? -1 : 1;
    }
    int c = memcmp(a->gm.ident, b->gm.ident, 4);
    if (c) {
        return c;
    }
    if (a->gm.variance != b->gm.variance) {
        return a->gm.variance < b->gm.variance ? -1 : 1;
    }
    c = memcmp(a->gm.gm.uuid, b->gm.gm.uuid, 6);
    return c ? c : (int)a->gm.gm.port - (int)b->gm.gm.port;
}

static void publish(void)
{
    s_locked = s_servo.locked && s_leader >= 0;
    int64_t o = s_servo.offset_ns;
    s_offset_ns = (int32_t)(o > INT32_MAX ? INT32_MAX : (o < INT32_MIN ? INT32_MIN : o));
    s_delay_ns = s_servo.delay_valid ? (int32_t)s_servo.delay_ns : 0;
    s_stage = s_leader < 0 ? 0 : (s_servo.stage == SERVO_TRACK ? 2 : (s_servo.stage == SERVO_FREQ ? 1 : 0));
}

static void forget_inflight(void)
{
    s_pending.valid = false;
    s_dreq.valid = false;
}

static void set_leader(int idx)
{
    bool was_locked = s_servo.locked;
    s_leader = idx;
    if (idx >= 0) {
        s_leader_id = s_cand[idx].src;
        s_have_leader = true;
    } else {
        s_have_leader = false;
    }
    s_leader_changes++;
    servo_restart(&s_servo);        // keeps the frequency (holdover)
    forget_inflight();
    s_next_dreq_ms = now_ms();
    publish();
    if (log_ok() || was_locked) {
        if (idx >= 0) {
            const uint8_t *u = s_leader_id.uuid;
            ESP_LOGI(TAG, "leader %02x%02x%02x%02x%02x%02x/%u (stratum %u)",
                     u[0], u[1], u[2], u[3], u[4], u[5], s_leader_id.port, s_cand[idx].gm.stratum);
        } else {
            ESP_LOGW(TAG, "leader lost (holding frequency %ld ppb)", (long)mc_freq_ppb());
        }
    }
}

static void select_leader(uint32_t t)
{
    int best = -1;
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (!s_cand[i].used) {
            continue;
        }
        if (t - s_cand[i].last_ms > CANDIDATE_TIMEOUT_MS) {
            s_cand[i].used = false;
            continue;
        }
        if (i != s_leader && s_cand[i].syncs < CANDIDATE_MIN_SYNCS) {
            continue;
        }
        if (best < 0 || bmc_cmp(&s_cand[i], &s_cand[best]) < 0) {
            best = i;
        }
    }
    if (best != s_leader) {
        set_leader(best);
    }
}

// ---- servo glue ------------------------------------------------------------
static void apply(const servo_act_t *a)
{
    bool was_locked = s_locked;
    if (a->step_ns) {
        mc_hw_step(a->step_ns);
        forget_inflight();          // timestamps taken before the step are void
        if (log_ok()) {
            ESP_LOGI(TAG, "step %lld ns (epoch %lu)", (long long)a->step_ns, (unsigned long)mc_epoch());
        }
    }
    if (a->set_freq) {
        mc_hw_set_freq_ppb(a->freq_ppb);
    }
    publish();
    if (s_locked != was_locked && log_ok()) {
        ESP_LOGI(TAG, "%s (offset %ld ns, delay %ld ns, %ld ppb)", s_locked ? "locked" : "unlocked",
                 (long)s_offset_ns, (long)s_delay_ns, (long)mc_freq_ppb());
    }
}

static void sample(int64_t t1, uint64_t t2)
{
    servo_act_t a;
    servo_sync(&s_servo, t1, (int64_t)t2, &a);
    apply(&a);
}

static void on_sync(const ptp_evt_t *ev, uint32_t t)
{
    if (memcmp(ev->src.uuid, s_uuid, 6) == 0) {
        return;
    }
    s_syncs++;
    int idx = -1, free_idx = -1;
    for (int i = 0; i < MAX_CANDIDATES; i++) {
        if (s_cand[i].used && port_eq(&s_cand[i].src, &ev->src)) {
            idx = i;
            break;
        }
        if (!s_cand[i].used && free_idx < 0) {
            free_idx = i;
        }
    }
    if (idx < 0) {
        if (free_idx < 0) {
            return;                 // more simultaneous senders than slots
        }
        idx = free_idx;
        memset(&s_cand[idx], 0, sizeof s_cand[idx]);
        s_cand[idx].used = true;
        s_cand[idx].src = ev->src;
    }
    s_cand[idx].gm = ev->gm;
    s_cand[idx].last_ms = t;
    s_cand[idx].syncs++;
    select_leader(t);

    if (idx != s_leader) {
        return;
    }
    if (ev->rx_ts == 0) {
        s_no_rx_ts++;
        s_pending.valid = false;
        return;
    }
    if (ev->epoch != mc_epoch()) {
        // Stamped before a step that the servo made while this Sync sat in
        // the queue: on the old timescale it would read as an offset of the
        // full step and step the clock straight back.
        s_pending.valid = false;
        return;
    }
    if (ev->two_step) {
        s_pending.valid = true;
        s_pending.seq = ev->seq;
        s_pending.t2 = ev->rx_ts;
    } else {
        s_pending.valid = false;
        sample(ev->ts, ev->rx_ts);
    }
}

static void on_follow_up(const ptp_evt_t *ev)
{
    s_fups++;
    if (s_leader < 0 || !port_eq(&ev->src, &s_leader_id) || !s_pending.valid || ev->seq != s_pending.seq) {
        return;
    }
    s_pending.valid = false;
    sample(ev->ts, s_pending.t2);
}

static void on_delay_resp(const ptp_evt_t *ev)
{
    s_dresps++;
    if (s_leader < 0 || !port_eq(&ev->src, &s_leader_id) || !s_dreq.valid || ev->dr.req_seq != s_dreq.seq) {
        return;
    }
    s_dreq.valid = false;
    servo_delay(&s_servo, (int64_t)s_dreq.t3, ev->ts);
    publish();
}

// Delay_Req: a Sync-format message (124 bytes) from us. The data set fields
// echo our parent's, as a slave port does.
static void send_delay_req(uint32_t t)
{
    const cand_t *ld = &s_cand[s_leader];
    uint8_t m[PTP_SYNC_LEN];
    memset(m, 0, sizeof m);
    wr16(m + 0, 1);                         // versionPTP
    wr16(m + 2, 1);                         // versionNetwork
    memcpy(m + 4, k_subdomain, sizeof k_subdomain);
    m[20] = PTP_MSG_EVENT;
    m[21] = PTP_COMM_ETHER;
    memcpy(m + 22, s_uuid, 6);
    wr16(m + 28, PTP_OUR_PORT_ID);
    uint16_t seq = ++s_dreq_seq;
    wr16(m + 30, seq);
    m[32] = PTP_CTL_DELAY_REQ;
    uint64_t now = mc_now_ns();             // approximate; t3 is the HW stamp
    wr32(m + 40, (uint32_t)(now / 1000000000ull));
    wr32(m + 44, (uint32_t)(now % 1000000000ull));
    m[53] = PTP_COMM_ETHER;                 // grandmaster data set
    memcpy(m + 54, ld->gm.gm.uuid, 6);
    wr16(m + 60, ld->gm.gm.port);
    m[67] = ld->gm.stratum;
    memcpy(m + 68, ld->gm.ident, 4);
    wr16(m + 74, (uint16_t)ld->gm.variance);
    m[77] = ld->gm.preferred;
    m[79] = ld->gm.boundary;
    m[83] = (uint8_t)ld->gm.sync_interval;
    wr16(m + 90, (uint16_t)(ld->gm.steps_removed + 1));     // localStepsRemoved
    m[95] = 255;                            // localClockStratum: follower only
    memcpy(m + 96, "DFLT", 4);              // localClockIdentifier
    m[101] = PTP_COMM_ETHER;                // parent
    memcpy(m + 102, ld->src.uuid, 6);
    wr16(m + 110, ld->src.port);

    uint64_t t3 = 0;
    esp_err_t err = net_udp_send_raw_ts(&s_dst_event, m, sizeof m, &t3);
    s_dreqs++;
    if (err != ESP_OK || t3 == 0) {
        if (err == ESP_OK) {
            s_no_tx_ts++;
        }
        s_dreq.valid = false;
        return;
    }
    s_dreq.valid = true;
    s_dreq.seq = seq;
    s_dreq.t3 = t3;
    s_dreq.sent_ms = t;
}

static void periodic(uint32_t t)
{
    if (s_leader >= 0 && t - s_cand[s_leader].last_ms > LEADER_TIMEOUT_MS) {
        s_cand[s_leader].used = false;
        set_leader(-1);
        select_leader(t);
    }
    if (s_dreq.valid && t - s_dreq.sent_ms > DREQ_TIMEOUT_MS) {
        s_dreq.valid = false;
    }
    if (s_leader >= 0 && s_servo.stage == SERVO_TRACK && net_link_up() &&
            (int32_t)(t - s_next_dreq_ms) >= 0) {
        send_delay_req(t);
        uint32_t iv = servo_want_fast_delay(&s_servo)
                      ? DREQ_FAST_MS
                      : DREQ_SLOW_MS * 3 / 4 + esp_random() % (DREQ_SLOW_MS / 2);
        s_next_dreq_ms = t + iv;
    }
}

static void ptp_task(void *arg)
{
    (void)arg;
    for (;;) {
        ptp_evt_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(20)) == pdTRUE) {
            uint32_t t = now_ms();
            switch (ev.kind) {
            case EV_SYNC:  on_sync(&ev, t); break;
            case EV_FUP:   on_follow_up(&ev); break;
            case EV_DRESP: on_delay_resp(&ev); break;
            default: break;
            }
        }
        periodic(now_ms());
    }
}

// ---- public (to mediaclock.c) ----------------------------------------------
void ptpv1_start(void)
{
    if (s_q) {
        return;
    }
    net_mac(s_uuid);
    servo_init(&s_servo);
    s_q = xQueueCreate(EVT_QUEUE_LEN, sizeof(ptp_evt_t));
    if (!s_q) {
        ESP_LOGE(TAG, "no memory for the event queue");
        return;
    }
    memcpy(&s_dst_event.dst_ip, k_group, 4);
    s_dst_event.dst_port = PTP_EVENT_PORT;
    s_dst_event.src_port = PTP_EVENT_PORT;
    s_dst_event.dscp = 56;                  // CS7
    net_udp_dest_resolve(&s_dst_event);     // multicast: immediate

    if (xTaskCreatePinnedToCore(ptp_task, "ptpv1", PTP_TASK_STACK, NULL, UI_PRIO_PTP,
                                NULL, UI_CORE_NET) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the PTP task");
        return;
    }
    if (net_udp_hook_register(PTP_EVENT_PORT, PTP_EVENT_PORT, ptp_hook, NULL) != ESP_OK ||
            net_udp_hook_register(PTP_GENERAL_PORT, PTP_GENERAL_PORT, ptp_hook, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "hook registration failed");
    }
    uint32_t g;
    memcpy(&g, k_group, 4);
    net_mcast_join(g);
    if (!mc_hw_ready()) {
        ESP_LOGW(TAG, "no 1588 hardware clock: PTP will not lock");
    }
}

bool ptpv1_locked(void) { return s_locked; }
int32_t ptpv1_offset_ns(void) { return s_offset_ns; }

void ptpv1_leader_id(uint8_t id[8])
{
    if (!s_have_leader) {
        memset(id, 0, 8);
        return;
    }
    port_id_t l = s_leader_id;      // may race a leader change; telemetry only
    memcpy(id, l.uuid, 6);
    id[6] = (uint8_t)(l.port >> 8);
    id[7] = (uint8_t)l.port;
}

void ptpv1_get_stats(mc_stats_t *s)
{
    memset(s, 0, sizeof *s);
    s->syncs = s_syncs;
    s->follow_ups = s_fups;
    s->delay_reqs = s_dreqs;
    s->delay_resps = s_dresps;
    s->no_hw_rx_ts = s_no_rx_ts;
    s->no_hw_tx_ts = s_no_tx_ts;
    s->queue_drops = atomic_load_explicit(&s_qdrops, memory_order_relaxed);
    s->steps = s_servo.st.steps;
    s->lock_losses = s_servo.st.lock_losses;
    s->outliers = s_servo.st.outliers;
    s->phase_shifts = s_servo.st.phase_shifts;
    s->leader_changes = s_leader_changes;
    s->delay_samples = s_servo.st.delay_samples;
    s->delay_rejected = s_servo.st.delay_rejected;
    s->path_delay_ns = s_delay_ns;
    s->stage = s_stage;
}
