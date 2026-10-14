// SPDX-License-Identifier: GPL-3.0-or-later
//
// Channel subscriber: turns "RX channel N <- <tx channel>@<tx device>"
// subscriptions into received flows. Translated from Inferno
// device_server/channels_subscriber.rs (ChannelsSubscriberInternal).
//
// One task (core 1). It owns the subscription and flow tables; the ARC
// server only enqueues commands and reads the per-channel status (under
// aoip_lock). Loop, as Inferno's run(): wait up to 2 s for commands, save
// state if any arrived, resolve when asked to (or every 9 s while something
// is unresolved), then scan flows.
//
// resolve: dealias channels subscribed to the same source, reuse sources
// already received, mDNS-resolve the rest (_netaudio-chan TXT + SRV), group
// them per transmitter (or per multicast bundle), reuse free slots of
// existing flows (flow-control "update flow"), join multicast bundles, and
// request new unicast flows of up to min(nchan, 8) channels.
// scan: a flow with no packets for 2 s (and older than 6 s) is torn down and
// its channels re-resolved, which also follows a transmitter that rebooted
// onto a new address; unused flows are stopped. Status changes are announced
// with the channel-change multicast so the controller turns the patch green.
//
// Subscriptions are persisted (NVS "subs") and restored at boot.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "app_config.h"
#include "aoip/aoip_internal.h"
#include "aoip/fpp.h"
#include "aoip/proto.h"

static const char *TAG = "aoip_sub";

#define NCH   UI_CHANNELS
#define FLOWS AOIP_RX_FLOW_SLOTS

typedef enum { CMD_SUBSCRIBE, CMD_UNSUBSCRIBE } cmd_type_t;

typedef struct {
    uint8_t type;
    uint8_t ch;
    char txch[AOIP_NAME_LEN];
    char host[AOIP_NAME_LEN];
} cmd_t;

typedef struct {
    bool     used;
    char     txch[AOIP_NAME_LEN];
    char     host[AOIP_NAME_LEN];
    int8_t   flow;              // -1: not connected to a flow (Inferno remote = None)
    uint8_t  cif;               // channel in flow
    uint16_t tx_id;
} sub_ch_t;

typedef struct {
    bool     used;
    bool     multicast;
    uint32_t ctl_ip;            // network order
    uint16_t ctl_port;
    uint16_t dbcp1;
    uint8_t  handle[FC_HANDLE_LEN];
    char     bundle[2 * AOIP_NAME_LEN + 8];
    uint8_t  nch;
    uint16_t tx_ch[AOIP_RX_MAX_CH_PER_FLOW];   // 0 = none
    int16_t  refcount[AOIP_RX_MAX_CH_PER_FLOW];
    uint32_t latency_frames;
    uint32_t created_ms;
} sub_flow_t;

static sub_ch_t s_ch[NCH];
static sub_flow_t s_fl[FLOWS];
static sub_info_t s_info[NCH];      // read by ARC under aoip_lock
static QueueHandle_t s_cmd;
static bool s_needs_resolving, s_resolve_now;

// ---------------------------------------------------------------------------
// Interface for the ARC server (ctl task)
// ---------------------------------------------------------------------------
void sub_subscribe(unsigned local_ch, const char *txch, const char *host)
{
    if (local_ch >= NCH || !s_cmd) return;
    cmd_t c = { .type = CMD_SUBSCRIBE, .ch = (uint8_t)local_ch };
    strncpy(c.txch, txch, AOIP_NAME_LEN - 1);
    strncpy(c.host, host, AOIP_NAME_LEN - 1);
    // Dante Controller asks for the RX channel list right after the reply,
    // so the status must already say "subscribed" (channels_subscriber.rs)
    aoip_lock();
    s_info[local_ch].present = true;
    strcpy(s_info[local_ch].txch, c.txch);
    strcpy(s_info[local_ch].host, c.host);
    s_info[local_ch].status = SUB_STATUS_UNRESOLVED;
    aoip_unlock();
    if (xQueueSend(s_cmd, &c, 0) != pdTRUE) ESP_LOGE(TAG, "command queue full");
}

void sub_unsubscribe(unsigned local_ch)
{
    if (local_ch >= NCH || !s_cmd) return;
    cmd_t c = { .type = CMD_UNSUBSCRIBE, .ch = (uint8_t)local_ch };
    if (xQueueSend(s_cmd, &c, 0) != pdTRUE) ESP_LOGE(TAG, "command queue full");
}

void sub_get_info(unsigned local_ch, sub_info_t *out)
{
    aoip_lock();
    if (local_ch < NCH) {
        *out = s_info[local_ch];
    } else {
        memset(out, 0, sizeof *out);
    }
    aoip_unlock();
}

void sub_counts(uint32_t *total, uint32_t *ok)
{
    uint32_t t = 0, o = 0;
    aoip_lock();
    for (unsigned i = 0; i < NCH; i++) {
        if (!s_info[i].present) continue;
        t++;
        o += s_info[i].status == SUB_STATUS_RX_UNICAST || s_info[i].status == SUB_STATUS_RX_MULTICAST;
    }
    aoip_unlock();
    *total = t;
    *ok = o;
}

// ---------------------------------------------------------------------------
// Internal (subscriber task)
// ---------------------------------------------------------------------------
static void set_status(unsigned ch, uint32_t status, uint32_t *changed)
{
    aoip_lock();
    sub_info_t *si = &s_info[ch];
    // Only touch the status of the subscription we worked on: the ARC server
    // may have replaced it meanwhile.
    if (si->present && strcmp(si->txch, s_ch[ch].txch) == 0 && strcmp(si->host, s_ch[ch].host) == 0) {
        if (si->status != status) *changed |= 1u << ch;
        si->status = status;
    }
    aoip_unlock();
}

static void save_state(void)
{
    static saved_sub_t subs[NCH];       // sub task only; 2 KB off its 6 KB stack
    memset(subs, 0, sizeof subs);
    for (unsigned i = 0; i < NCH; i++) {
        if (!s_ch[i].used) continue;
        strcpy(subs[i].txch, s_ch[i].txch);
        strcpy(subs[i].host, s_ch[i].host);
    }
    settings_set_subs(subs);
}

static void unsubscribe_internal(unsigned ch, bool remove_info)
{
    sub_ch_t *c = &s_ch[ch];
    if (!c->used) {
        ESP_LOGW(TAG, "unsubscribe of not subscribed channel %u", ch + 1);
        if (remove_info) {
            aoip_lock();
            s_info[ch].present = false;
            aoip_unlock();
        }
        return;
    }
    if (c->flow >= 0) {
        sub_flow_t *f = &s_fl[c->flow];
        frx_disconnect_channel((unsigned)c->flow, c->cif, ch);
        if (f->used && c->cif < f->nch) f->refcount[c->cif]--;
    }
    c->used = false;
    c->flow = -1;
    if (remove_info) {
        aoip_lock();
        s_info[ch].present = false;
        s_info[ch].status = SUB_STATUS_NONE;
        aoip_unlock();
    }
    info_notify_rx_channels(1u << ch);
}

static void subscribe_internal(unsigned ch, const char *txch, const char *host)
{
    if (s_ch[ch].used) unsubscribe_internal(ch, false);
    sub_ch_t *c = &s_ch[ch];
    memset(c, 0, sizeof *c);
    c->used = true;
    strcpy(c->txch, txch);
    strcpy(c->host, host);
    c->flow = -1;
    // An UNSUBSCRIBE queued before this command (ARC: unsubscribe, then
    // subscribe quickly) has just wiped the status that sub_subscribe() set:
    // restore it, or ARC reports the channel unpatched while it is patched.
    aoip_lock();
    if (!s_info[ch].present) {
        s_info[ch].present = true;
        strcpy(s_info[ch].txch, txch);
        strcpy(s_info[ch].host, host);
        s_info[ch].status = SUB_STATUS_UNRESOLVED;
    }
    aoip_unlock();
    s_needs_resolving = true;
    s_resolve_now = true;
}

static int free_flow_slot(void)
{
    for (unsigned i = 0; i < FLOWS; i++) {
        if (!s_fl[i].used) return (int)i;
    }
    return -1;
}

// Connect all channels of an alias group to (flow, cif).
static void apply_update(uint32_t aliases, unsigned flow, unsigned cif, uint16_t tx_id, uint32_t *changed)
{
    sub_flow_t *f = &s_fl[flow];
    for (unsigned ch = 0; ch < NCH; ch++) {
        if (!(aliases & (1u << ch))) continue;
        if (cif >= f->nch) {
            ESP_LOGE(TAG, "channel in flow %u out of range (%u)", cif, f->nch);
            continue;
        }
        if (!f->multicast) f->tx_ch[cif] = tx_id;
        f->refcount[cif]++;
        frx_connect_channel(flow, cif, ch);
        s_ch[ch].flow = (int8_t)flow;
        s_ch[ch].cif = (uint8_t)cif;
        s_ch[ch].tx_id = tx_id;
        set_status(ch, SUB_STATUS_IN_PROGRESS, changed);
    }
}

static uint32_t latency_frames(uint32_t adv_min_latency_ns)
{
    uint64_t ns = adv_min_latency_ns > UI_NET_LATENCY_NS ? adv_min_latency_ns : UI_NET_LATENCY_NS;
    return (uint32_t)(ns * UI_SAMPLE_RATE / 1000000000ull);
}

static void resolve_subscriptions(void)
{
    s_resolve_now = false;
    uint32_t changed = 0;

    // channels we need to connect, dealiased: rep -> mask of channels with the same source
    uint32_t alias[NCH] = { 0 };
    uint32_t need = 0;
    for (unsigned i = 0; i < NCH; i++) {
        if (!s_ch[i].used || s_ch[i].flow >= 0) continue;
        unsigned rep = i;
        for (unsigned j = 0; j < i; j++) {
            if ((need & (1u << j)) && strcmp(s_ch[j].txch, s_ch[i].txch) == 0 &&
                strcmp(s_ch[j].host, s_ch[i].host) == 0) {
                rep = j;
                break;
            }
        }
        if (rep == i) need |= 1u << i;
        alias[rep] |= 1u << i;
    }
    if (!need) {
        s_needs_resolving = false;
        return;
    }

    // reuse sources some other channel already receives
    for (unsigned i = 0; i < NCH; i++) {
        if (!(need & (1u << i))) continue;
        for (unsigned j = 0; j < NCH; j++) {
            if (s_ch[j].used && s_ch[j].flow >= 0 && strcmp(s_ch[j].txch, s_ch[i].txch) == 0 &&
                strcmp(s_ch[j].host, s_ch[i].host) == 0) {
                apply_update(alias[i], (unsigned)s_ch[j].flow, s_ch[j].cif, s_ch[j].tx_id, &changed);
                need &= ~(1u << i);
                break;
            }
        }
    }

    // mDNS: resolve the remaining channels
    static char host[NCH][AOIP_NAME_LEN], chan[NCH][AOIP_NAME_LEN];
    static aoip_adv_chan_t adv[NCH];
    static bool ok[NCH];
    uint8_t rep_of[NCH];
    unsigned nq = 0;
    for (unsigned i = 0; i < NCH; i++) {
        if (!(need & (1u << i))) continue;
        strcpy(host[nq], s_ch[i].host);
        strcpy(chan[nq], s_ch[i].txch);
        rep_of[nq++] = (uint8_t)i;
    }
    if (nq) aoip_mdns_query_chans((const char (*)[AOIP_NAME_LEN])host, (const char (*)[AOIP_NAME_LEN])chan,
                                  nq, adv, ok);

    // group per server (unicast) and per bundle (multicast)
    // "Check for surprises": one server must agree on nchan, bits and dbcp1
    for (unsigned a = 0; a < nq; a++) {
        if (!ok[a] || adv[a].has_bundle) continue;
        if (adv[a].nchan == 0 || adv[a].tx_channel_id == 0 || adv[a].bits % 8 || adv[a].bits < 16 ||
            adv[a].bits > 32) {
            ok[a] = false;
            continue;
        }
        for (unsigned b = 0; b < nq; b++) {
            if (b == a || !ok[b] || adv[b].has_bundle) continue;
            if (adv[b].ip == adv[a].ip && adv[b].port == adv[a].port &&
                (adv[b].nchan != adv[a].nchan || adv[b].bits != adv[a].bits || adv[b].dbcp1 != adv[a].dbcp1)) {
                ESP_LOGW(TAG, "server has different nchan/bits/dbcp1 per channel: unsupported");
                for (unsigned c = 0; c < nq; c++) {
                    if (adv[c].ip == adv[a].ip && adv[c].port == adv[a].port && !adv[c].has_bundle) ok[c] = false;
                }
                break;
            }
        }
    }

    // existing flows: needed channels already present in a flow, free slots
    bool done[NCH] = { 0 };
    for (unsigned fl = 0; fl < FLOWS; fl++) {
        sub_flow_t *f = &s_fl[fl];
        if (!f->used) continue;
        for (unsigned a = 0; a < nq; a++) {
            if (!ok[a] || done[a]) continue;
            if (!f->multicast && !adv[a].has_bundle && adv[a].ip == f->ctl_ip && adv[a].port == f->ctl_port) {
                for (unsigned k = 0; k < f->nch; k++) {
                    if (f->tx_ch[k] == adv[a].tx_channel_id) {
                        apply_update(alias[rep_of[a]], fl, k, adv[a].tx_channel_id, &changed);
                        done[a] = true;
                        break;
                    }
                }
            } else if (f->multicast && adv[a].has_bundle) {
                char bname[sizeof f->bundle];
                snprintf(bname, sizeof bname, "%u@%.31s", adv[a].bundle_id, host[a]);
                if (strcmp(bname, f->bundle) == 0) {
                    apply_update(alias[rep_of[a]], fl, adv[a].channel_in_bundle, adv[a].tx_channel_id, &changed);
                    done[a] = true;
                }
            }
        }
    }

    // free slots of existing unicast flows: "update flow" with the new list
    for (unsigned fl = 0; fl < FLOWS; fl++) {
        sub_flow_t *f = &s_fl[fl];
        if (!f->used || f->multicast) continue;
        uint16_t new_tx[AOIP_RX_MAX_CH_PER_FLOW];
        memcpy(new_tx, f->tx_ch, sizeof new_tx);
        int assign_a[AOIP_RX_MAX_CH_PER_FLOW];
        bool any = false;
        for (unsigned k = 0; k < f->nch; k++) {
            assign_a[k] = -1;
            if (f->refcount[k] > 0) continue;
            for (unsigned a = 0; a < nq; a++) {
                if (!ok[a] || done[a] || adv[a].has_bundle || adv[a].ip != f->ctl_ip || adv[a].port != f->ctl_port) continue;
                bool taken = false;
                for (unsigned j = 0; j < k; j++) taken |= assign_a[j] == (int)a;
                if (taken) continue;
                assign_a[k] = (int)a;
                new_tx[k] = adv[a].tx_channel_id;
                any = true;
                break;
            }
        }
        if (!any) continue;
        uint16_t code = fcc_update_flow(f->ctl_ip, f->ctl_port, f->handle, new_tx, f->nch);
        if (code != RR_CODE_OK) {
            ESP_LOGW(TAG, "update flow %u failed (0x%04x)", fl + 1, code);
            continue;
        }
        for (unsigned k = 0; k < f->nch; k++) {
            if (assign_a[k] < 0) continue;
            unsigned a = (unsigned)assign_a[k];
            if (f->tx_ch[k] && f->tx_ch[k] != adv[a].tx_channel_id) {
                ESP_LOGW(TAG, "reusing channel %u in flow %u for tx channel %u", k, fl + 1, adv[a].tx_channel_id);
            }
            apply_update(alias[rep_of[a]], fl, k, adv[a].tx_channel_id, &changed);
            done[a] = true;
        }
    }

    // new multicast flows (bundles)
    for (unsigned a = 0; a < nq; a++) {
        if (!ok[a] || done[a] || !adv[a].has_bundle) continue;
        char bname[2 * AOIP_NAME_LEN + 8];
        snprintf(bname, sizeof bname, "%u@%.31s", adv[a].bundle_id, host[a]);
        aoip_adv_bundle_t bund;
        if (!aoip_mdns_query_bundle(bname, &bund)) {
            ESP_LOGW(TAG, "could not resolve bundle %s", bname);
            continue;
        }
        if (bund.nchan == 0 || bund.nchan > AOIP_RX_MAX_CH_PER_FLOW || bund.bits % 8 || bund.bits < 16 ||
            bund.bits > 32) {
            ESP_LOGW(TAG, "bundle %s unsupported (%u ch, %lu bit)", bname, bund.nchan, (unsigned long)bund.bits);
            continue;
        }
        int fl = free_flow_slot();
        if (fl < 0) {
            ESP_LOGW(TAG, "no free RX flow slot");
            break;
        }
        sub_flow_t *f = &s_fl[fl];
        memset(f, 0, sizeof *f);
        f->multicast = true;
        strcpy(f->bundle, bname);
        f->nch = (uint8_t)bund.nchan;
        f->latency_frames = latency_frames(bund.min_latency_ns);
        f->created_ms = aoip_now_ms();
        if (frx_add_flow((unsigned)fl, bund.ip, bund.bits / 8u, f->nch, f->latency_frames, false) != ESP_OK) {
            continue;
        }
        f->used = true;
        ESP_LOGI(TAG, "receiving multicast bundle %s on flow %d", bname, fl + 1);
        // every needed channel of this bundle
        for (unsigned b = a; b < nq; b++) {
            if (!ok[b] || done[b] || !adv[b].has_bundle || adv[b].bundle_id != adv[a].bundle_id ||
                strcmp(host[b], host[a]) != 0) continue;
            apply_update(alias[rep_of[b]], (unsigned)fl, adv[b].channel_in_bundle, adv[b].tx_channel_id, &changed);
            done[b] = true;
        }
    }

    // new unicast flows: per server, chunks of min(nchan, channels, 8)
    for (unsigned a = 0; a < nq; a++) {
        if (!ok[a] || done[a] || adv[a].has_bundle) continue;
        const aoip_adv_chan_t first = adv[a];
        unsigned per_flow = first.nchan;
        if (per_flow > NCH) per_flow = NCH;
        if (per_flow > RX_MAX_CH_PER_REQUESTED_FLOW) per_flow = RX_MAX_CH_PER_REQUESTED_FLOW;
        // collect this server's remaining channels
        unsigned list[NCH], nl = 0;
        for (unsigned b = a; b < nq; b++) {
            if (ok[b] && !done[b] && !adv[b].has_bundle && adv[b].ip == first.ip && adv[b].port == first.port) {
                list[nl++] = b;
            }
        }
        for (unsigned start = 0; start < nl; start += per_flow) {
            unsigned cnt = nl - start < per_flow ? nl - start : per_flow;
            int fl = free_flow_slot();
            if (fl < 0) {
                ESP_LOGW(TAG, "no free RX flow slot");
                break;
            }
            sub_flow_t *f = &s_fl[fl];
            memset(f, 0, sizeof *f);
            f->ctl_ip = first.ip;
            f->ctl_port = first.port;
            f->dbcp1 = first.dbcp1;
            f->nch = (uint8_t)per_flow;
            for (unsigned k = 0; k < cnt; k++) f->tx_ch[k] = adv[list[start + k]].tx_channel_id;
            f->latency_frames = latency_frames(first.min_latency_ns);
            f->created_ms = aoip_now_ms();
            const unsigned bps = first.bits / 8u;
            unsigned fpp_mtu = RX_MAX_PAYLOAD_BYTES / (per_flow * bps);
            unsigned fpp;
            if (first.fpp_min > fpp_mtu) {
                // The transmitter needs bigger packets than fit an
                // unfragmented frame: they would fragment and never reach
                // the audio hook (net.c drops fragments), so this flow
                // could only ever deliver silence. Refuse it instead.
                ESP_LOGW(TAG, "transmitter minimum fpp %u exceeds MTU cap %u (" IPSTR ":%u)",
                         first.fpp_min, fpp_mtu,
                         IP2STR((esp_ip4_addr_t *)&first.ip), first.port);
                for (unsigned k = 0; k < cnt; k++) {
                    uint32_t al = alias[rep_of[list[start + k]]];
                    for (unsigned ch = 0; ch < NCH; ch++) {
                        if (al & (1u << ch)) set_status(ch, SUB_STATUS_TX_FAIL, &changed);
                    }
                }
                continue;
            }
            fpp = aoip_select_fpp(first.fpp_min, first.fpp_max, fpp_mtu, UI_TX_FPP_MAX);
            if (fpp > UI_TX_FPP_MAX) {
                // Above only the latency cap: request the transmitter's
                // minimum anyway (degraded; the shortfall shows in rx_late).
                ESP_LOGW(TAG, "transmitter minimum fpp %u exceeds latency cap %u (" IPSTR ":%u)",
                         first.fpp_min, UI_TX_FPP_MAX,
                         IP2STR((esp_ip4_addr_t *)&first.ip), first.port);
            }
            if (fpp == 0) fpp = 1;
            char flow_name[16];
            snprintf(flow_name, sizeof flow_name, "%d_%u", fl + 1, g_aoip.process_id);

            if (frx_add_flow((unsigned)fl, 0, bps, per_flow, f->latency_frames, true) != ESP_OK) break;
            f->used = true;
            uint16_t code = fcc_request_flow(first.ip, first.port, UI_SAMPLE_RATE, first.bits, (uint16_t)fpp,
                                             f->tx_ch, per_flow, frx_port((unsigned)fl), flow_name, f->handle);
            if (code != RR_CODE_OK) {
                ESP_LOGW(TAG, "flow request to " IPSTR ":%u failed (0x%04x)",
                         IP2STR((esp_ip4_addr_t *)&first.ip), first.port, code);
                for (unsigned k = 0; k < cnt; k++) {
                    uint32_t al = alias[rep_of[list[start + k]]];
                    for (unsigned ch = 0; ch < NCH; ch++) {
                        if (al & (1u << ch)) set_status(ch, SUB_STATUS_TX_FAIL, &changed);
                    }
                }
                frx_remove_flow((unsigned)fl);
                memset(f, 0, sizeof *f);
                continue;
            }
            ESP_LOGI(TAG, "flow %d: %u ch from " IPSTR " fpp %u, latency %lu frames", fl + 1, cnt,
                     IP2STR((esp_ip4_addr_t *)&first.ip), fpp, (unsigned long)f->latency_frames);
            for (unsigned k = 0; k < cnt; k++) {
                unsigned b = list[start + k];
                apply_update(alias[rep_of[b]], (unsigned)fl, k, adv[b].tx_channel_id, &changed);
                done[b] = true;
            }
        }
        for (unsigned k = 0; k < nl; k++) done[list[k]] = true;   // tried
    }

    if (changed) info_notify_rx_channels(changed);
}

static void scan_flows(void)
{
    const uint32_t now = aoip_now_ms();
    uint32_t changed = 0;
    for (unsigned fl = 0; fl < FLOWS; fl++) {
        sub_flow_t *f = &s_fl[fl];
        if (!f->used) continue;
        bool in_use = false;
        for (unsigned k = 0; k < f->nch; k++) in_use |= f->refcount[k] > 0;
        bool remove = false;
        if (in_use) {
            uint32_t last = frx_last_packet_ms(fl);
            bool stale = last == 0 || now - last > RX_FLOW_TIMEOUT_S * 1000u;
            bool receiving = !stale;
            if (stale && now - f->created_ms > RX_FLOW_MIN_AGE_S * 1000u) {
                ESP_LOGW(TAG, "flow %u timeout (no media packets)", fl + 1);
                remove = true;
            }
            for (unsigned ch = 0; ch < NCH; ch++) {
                if (!s_ch[ch].used || s_ch[ch].flow != (int)fl) continue;
                if (remove) {
                    frx_disconnect_channel(fl, s_ch[ch].cif, ch);
                    s_ch[ch].flow = -1;
                    s_needs_resolving = true;
                    s_resolve_now = true;
                    set_status(ch, SUB_STATUS_UNRESOLVED, &changed);
                } else if (receiving) {
                    set_status(ch, f->multicast ? SUB_STATUS_RX_MULTICAST : SUB_STATUS_RX_UNICAST, &changed);
                }
            }
        } else {
            remove = true;
        }
        if (remove) {
            if (!f->multicast) {
                ESP_LOGI(TAG, "stopping flow %u at " IPSTR, fl + 1, IP2STR((esp_ip4_addr_t *)&f->ctl_ip));
                // errors ignored: without keepalives the transmitter drops it anyway
                fcc_stop_flow(f->ctl_ip, f->ctl_port, f->handle);
            }
            frx_remove_flow(fl);
            memset(f, 0, sizeof *f);
        }
    }
    if (changed) info_notify_rx_channels(changed);
}

static void load_state(void)
{
    static saved_sub_t subs[NCH];       // sub task only (stack)
    settings_get_subs(subs);
    for (unsigned i = 0; i < NCH; i++) {
        if (!subs[i].txch[0] || !subs[i].host[0]) continue;
        aoip_lock();
        s_info[i].present = true;
        strcpy(s_info[i].txch, subs[i].txch);
        strcpy(s_info[i].host, subs[i].host);
        s_info[i].status = SUB_STATUS_UNRESOLVED;
        aoip_unlock();
        subscribe_internal(i, subs[i].txch, subs[i].host);
        ESP_LOGI(TAG, "restored subscription %u <- %s@%s", i + 1, subs[i].txch, subs[i].host);
    }
}

static void sub_task(void *arg)
{
    load_state();
    uint32_t last_resolve = aoip_now_ms();
    for (;;) {
        cmd_t c;
        bool received = false;
        TickType_t wait = pdMS_TO_TICKS(2000);
        while (xQueueReceive(s_cmd, &c, wait) == pdTRUE) {
            wait = 0;
            received = true;
            if (c.ch >= NCH) continue;
            if (c.type == CMD_SUBSCRIBE) {
                subscribe_internal(c.ch, c.txch, c.host);
            } else {
                unsubscribe_internal(c.ch, true);
            }
        }
        if (received) save_state();
        // wait for an address before talking to anyone
        if (atomic_load(&g_aoip.ip) == 0) continue;
        uint32_t now = aoip_now_ms();
        if ((s_needs_resolving && now - last_resolve >= RX_RESOLVE_INTERVAL_S * 1000u) || s_resolve_now) {
            resolve_subscriptions();
            last_resolve = aoip_now_ms();
        }
        scan_flows();
    }
}

esp_err_t sub_start(void)
{
    for (unsigned i = 0; i < NCH; i++) s_ch[i].flow = -1;
    s_cmd = xQueueCreate(64, sizeof(cmd_t));
    if (!s_cmd) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreatePinnedToCore(sub_task, "aoip_sub", 6144, NULL, UI_PRIO_CONTROL - 1,
                                            NULL, UI_CORE_USB);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
