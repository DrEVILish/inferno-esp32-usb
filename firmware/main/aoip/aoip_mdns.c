// SPDX-License-Identifier: GPL-3.0-or-later
//
// mDNS advertisement and lookups (espressif/mdns). Translated from Inferno
// device_server/mdns_server.rs (what we publish) and mdns_client.rs (how we
// resolve transmitters' channels and multicast bundles).
//
// Publishing is espressif-mdns-specific; the TXT parsing of the lookup
// side lives in aoip/mdns_txt.c and is host-tested.
//
// Published (hostname = friendly device name):
//   <host>._netaudio-arc._udp    port 4440  arcp_vers, arcp_min, router_vers, router_info, mf, model
//   <host>._netaudio-cmc._udp    port 8800  id (EUI-64), process, cmcp_vers, ..., channels, mf, model, "", ""
//   <ch>@<host>._netaudio-chan._udp  port 4455, one per TX channel factory name
//                                (with "default") plus one per friendly name if different
//   <id>@<host>._netaudio-bund._udp  port 4455, one per active multicast TX flow
//
// espressif/mdns builds TXT lists by prepending, so arrays are passed
// reversed to keep Inferno's TXT order on the wire.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mdns.h"

#include "app_config.h"
#include "net/net.h"
#include "aoip/aoip_internal.h"
#include "aoip/aoip.h"
#include "aoip/mdns_txt.h"
#include "aoip/proto.h"

static const char *TAG = "aoip_mdns";

#define MAX_TXT 16
#define CHAN_SERVICE "_netaudio-chan"
#define BUND_SERVICE "_netaudio-bund"
#define PROTO_UDP    "_udp"

static bool s_up;

typedef struct {
    mdns_txt_item_t items[MAX_TXT];
    char store[MAX_TXT][40];
    size_t n;
} txt_list_t;

static void txt_add(txt_list_t *t, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void txt_add(txt_list_t *t, const char *key, const char *fmt, ...)
{
    if (t->n >= MAX_TXT) return;
    mdns_txt_item_t *it = &t->items[t->n];
    it->key = key;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(t->store[t->n], sizeof t->store[t->n], fmt, ap);
        va_end(ap);
        it->value = t->store[t->n];
    } else {
        it->value = NULL;
    }
    t->n++;
}

static void txt_reverse(txt_list_t *t)
{
    for (size_t i = 0; i < t->n / 2; i++) {
        mdns_txt_item_t tmp = t->items[i];
        t->items[i] = t->items[t->n - 1 - i];
        t->items[t->n - 1 - i] = tmp;
    }
}

esp_err_t aoip_mdns_init(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    s_up = true;
    return ESP_OK;
}

void aoip_mdns_ip_changed(void)
{
    if (!s_up) return;
    esp_netif_t *nif = net_netif();
    if (!nif) return;
    esp_err_t err = mdns_register_netif(nif);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "register netif: %s", esp_err_to_name(err));
        return;
    }
    mdns_netif_action(nif, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4);
}

static void add_chan_service(unsigned index, const char *ch_name, bool is_default)
{
    char inst[2 * AOIP_NAME_LEN + 2];
    snprintf(inst, sizeof inst, "%s@%s", ch_name, g_aoip.friendly_hostname);
    txt_list_t t = { 0 };
    txt_add(&t, "txtvers", "2");
    txt_add(&t, "dbcp1", MDNS_CHAN_TXT_DBCP1);
    txt_add(&t, "dbcp", MDNS_CHAN_TXT_DBCP);
    txt_add(&t, "id", "%u", index + 1u);
    txt_add(&t, "rate", "%u", UI_SAMPLE_RATE);
    txt_add(&t, "pcm", "%u %x", UI_NET_BITS / 8u, AOIP_PCM_TYPE);
    txt_add(&t, "enc", "%u", UI_NET_BITS);
    txt_add(&t, "en", "%u", UI_NET_BITS);
    txt_add(&t, "latency_ns", "%u", UI_NET_LATENCY_NS);
    txt_add(&t, "fpp", "%u,%u", TX_FPP_MAX_ADVERTISED, TX_FPP_MIN);
    txt_add(&t, "nchan", "%u", UI_CHANNELS < UI_TX_MAX_CH_PER_FLOW ? UI_CHANNELS : UI_TX_MAX_CH_PER_FLOW);
    if (is_default) txt_add(&t, "default", NULL);
    unsigned bundle, chb;
    static char bkey[12];
    if (txm_channel_bundle(index, &bundle, &chb)) {
        snprintf(bkey, sizeof bkey, "b.%u", bundle);
        txt_add(&t, bkey, "%u", chb + 1u);
    }
    txt_reverse(&t);
    esp_err_t err = mdns_service_add_for_host(inst, CHAN_SERVICE, PROTO_UDP, NULL,
                                              AOIP_PORT_FLOWS_CONTROL, t.items, t.n);
    if (err != ESP_OK) ESP_LOGE(TAG, "add %s: %s", inst, esp_err_to_name(err));
}

static void remove_chan_service(const char *ch_name)
{
    char inst[2 * AOIP_NAME_LEN + 2];
    snprintf(inst, sizeof inst, "%s@%s", ch_name, g_aoip.friendly_hostname);
    mdns_service_remove_for_host(inst, CHAN_SERVICE, PROTO_UDP, NULL);
}

static void add_tx_channel(unsigned index)
{
    char fac[4];
    aoip_factory_ch_name(fac, index);
    add_chan_service(index, fac, true);
    if (strcmp(fac, g_aoip.tx_names[index]) != 0) add_chan_service(index, g_aoip.tx_names[index], false);
}

void aoip_mdns_remove_tx_channel_name(unsigned index, const char *name)
{
    if (!s_up) return;
    char fac[4];
    aoip_factory_ch_name(fac, index);
    if (strcmp(fac, name) != 0) remove_chan_service(name);
}

void aoip_mdns_refresh_tx_channel(unsigned index)
{
    if (!s_up || index >= UI_CHANNELS) return;
    char fac[4];
    aoip_factory_ch_name(fac, index);
    remove_chan_service(fac);
    if (strcmp(fac, g_aoip.tx_names[index]) != 0) remove_chan_service(g_aoip.tx_names[index]);
    add_tx_channel(index);
}

void aoip_mdns_publish_all(void)
{
    if (!s_up) return;
    mdns_service_remove_all();
    mdns_hostname_set(g_aoip.friendly_hostname);
    mdns_instance_name_set(g_aoip.friendly_hostname);

    txt_list_t t = { 0 };
    txt_add(&t, "arcp_vers", "%s", MDNS_ARC_TXT_ARCP_VERS + strlen("arcp_vers="));
    txt_add(&t, "arcp_min", "%s", MDNS_ARC_TXT_ARCP_MIN + strlen("arcp_min="));
    txt_add(&t, "router_vers", "%s", MDNS_ARC_TXT_ROUTER_VERS + strlen("router_vers="));
    txt_add(&t, "router_info", "%s", AOIP_BOARD_NAME);
    txt_add(&t, "mf", "%s", UI_MANUFACTURER);
    txt_add(&t, "model", "%s", AOIP_MODEL_NUMBER);
    txt_reverse(&t);
    esp_err_t err = mdns_service_add(g_aoip.friendly_hostname, "_netaudio-arc", PROTO_UDP,
                                     AOIP_PORT_ARC, t.items, t.n);
    if (err != ESP_OK) ESP_LOGE(TAG, "add arc: %s", esp_err_to_name(err));

    memset(&t, 0, sizeof t);
    const uint8_t *d = g_aoip.devid;
    txt_add(&t, "id", "%02x%02x%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    txt_add(&t, "process", "%u", g_aoip.process_id);
    txt_add(&t, "cmcp_vers", "%s", MDNS_CMC_TXT_CMCP_VERS + strlen("cmcp_vers="));
    txt_add(&t, "cmcp_min", "%s", MDNS_CMC_TXT_CMCP_MIN + strlen("cmcp_min="));
    txt_add(&t, "server_vers", "%s", MDNS_CMC_TXT_SERVER_VERS + strlen("server_vers="));
    txt_add(&t, "channels", "%s", MDNS_CMC_TXT_CHANNELS + strlen("channels="));
    txt_add(&t, "mf", "%s", UI_MANUFACTURER);
    txt_add(&t, "model", "%s", AOIP_MODEL_NUMBER);
    txt_add(&t, "", NULL);              // "really needed?"
    txt_add(&t, "", NULL);              // "really needed?"
    txt_reverse(&t);
    err = mdns_service_add(g_aoip.friendly_hostname, "_netaudio-cmc", PROTO_UDP,
                           AOIP_PORT_CMC, t.items, t.n);
    if (err != ESP_OK) ESP_LOGE(TAG, "add cmc: %s", esp_err_to_name(err));

    for (unsigned i = 0; i < UI_CHANNELS; i++) add_tx_channel(i);
    txm_mdns_republish();
}

// Deferred full republish: a device rename (ARC 0x1001) runs under the
// serve() aoip_lock, and publish_all() performs ~70 mDNS operations. The
// handler only sets the flag; the ctl loop performs the republish after
// releasing the lock. Both run in the ctl task: no locking needed.
static bool s_republish_pending;

void aoip_mdns_request_republish(void)
{
    s_republish_pending = true;
}

void aoip_mdns_publish_if_pending(void)
{
    if (!s_republish_pending) return;
    s_republish_pending = false;
    aoip_mdns_publish_all();
}

void aoip_mdns_add_bundle(unsigned bundle_id, unsigned nch, unsigned fpp, uint32_t ip, uint16_t port)
{
    if (!s_up) return;
    char inst[AOIP_NAME_LEN + 8];
    snprintf(inst, sizeof inst, "%u@%s", bundle_id, g_aoip.friendly_hostname);
    const uint8_t *a = (const uint8_t *)&ip;
    txt_list_t t = { 0 };
    txt_add(&t, "txtvers", "1");
    txt_add(&t, "id", "%u", bundle_id);
    txt_add(&t, "nchan", "%u", nch);
    txt_add(&t, "latency_ns", "%u", UI_NET_LATENCY_NS);
    txt_add(&t, "fpp", "%u", fpp);
    txt_add(&t, "rate", "%u", UI_SAMPLE_RATE);
    txt_add(&t, "enc", "%u", UI_NET_BITS);
    txt_add(&t, "a.0", "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    txt_add(&t, "p.0", "%u", port);
    txt_reverse(&t);
    esp_err_t err = mdns_service_add_for_host(inst, BUND_SERVICE, PROTO_UDP, NULL,
                                              AOIP_PORT_FLOWS_CONTROL, t.items, t.n);
    if (err != ESP_OK) ESP_LOGE(TAG, "add %s: %s", inst, esp_err_to_name(err));
}

void aoip_mdns_remove_bundle(unsigned bundle_id)
{
    if (!s_up) return;
    char inst[AOIP_NAME_LEN + 8];
    snprintf(inst, sizeof inst, "%u@%s", bundle_id, g_aoip.friendly_hostname);
    mdns_service_remove_for_host(inst, BUND_SERVICE, PROTO_UDP, NULL);
}

// ---------------------------------------------------------------------------
// Lookups (mdns_client.rs). aoip_mdns.c adapts espressif/mdns results into
// aoip_txt_kv_t lists; the parsing itself lives in mdns_txt.c (host-testable,
// range-checked before any narrowing).
// ---------------------------------------------------------------------------

// Copy a result's TXT records into a view for mdns_txt_parse_*. Cap 32: our
// publisher sends at most 13, well-behaved peers stay near that; a peer
// with more keys yields the first 32 (a missing "b.*" key there only means
// "no bundle reference" and is re-checked on the next resolve).
static size_t txt_view(const mdns_result_t *r, aoip_txt_kv_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; r && i < r->txt_count && n < cap; i++) {
        if (!r->txt[i].key) continue;
        out[n].key = r->txt[i].key;
        out[n].value = r->txt[i].value;
        out[n].value_len =
            r->txt_value_len ? r->txt_value_len[i] : (r->txt[i].value ? strlen(r->txt[i].value) : 0);
        n++;
    }
    return n;
}

#define QUERY_TIMEOUT_MS 3000u

// Hard deadlines for one resolve cycle. The old code queried in batches of
// 8 and fell back to one *serial* 3 s A-record lookup per host whose SRV
// answer carried no address: 32 channels on 32 distinct unresolved hosts
// took up to ~96 s and stalled the subscriber task (so the command queue
// backed up). Now all channels are asked at once and gathered under one
// deadline; the A-record round for hosts without an SRV address runs in
// parallel under its own deadline. Searches that outlive their window are
// parked and reaped at the start of the next cycle, so nothing leaks.
#define RESOLVE_SRV_MS 4200u    // SRV + TXT answer window
#define RESOLVE_A_MS   2800u    // A-record window for hosts without an SRV IP

// A-record fallback cache: one badly-announcing host subscribed on many
// channels used to cost one 3 s mdns_query_a() per channel (~100 s for 32).
// The SRV answers for those channels all carry the same hostname, so the
// first fallback resolves it and the rest reuse the address. Entries
// expire after HOST_IP_CACHE_MS (a transmitter that changes address is
// picked up again within that window) and are refreshed whenever an SRV
// answer already carries an IPv4 address. Subscriber task only (the sole
// caller of aoip_mdns_query_chans); no locking.
#define HOST_IP_CACHE_N  8u
#define HOST_IP_CACHE_MS (2u * 60u * 1000u)

typedef struct {
    char     host[AOIP_NAME_LEN];
    uint32_t ip;        // net order, 0 = slot free
    uint32_t seen_ms;   // aoip_now_ms() when stored or refreshed
} host_ip_cache_t;

static host_ip_cache_t s_host_cache[HOST_IP_CACHE_N];
static unsigned s_host_cache_victim;

static bool host_cache_lookup(const char *hostname, uint32_t *ip_out)
{
    const uint32_t now = aoip_now_ms();
    for (unsigned i = 0; i < HOST_IP_CACHE_N; i++) {
        if (s_host_cache[i].ip == 0 || strcmp(s_host_cache[i].host, hostname) != 0) continue;
        if (now - s_host_cache[i].seen_ms >= HOST_IP_CACHE_MS) {
            s_host_cache[i].ip = 0;     // expired: resolve again next cycle
            return false;
        }
        *ip_out = s_host_cache[i].ip;
        return true;
    }
    return false;
}

static void host_cache_store(const char *hostname, uint32_t ip)
{
    // Only storable names: a name that does not fit the key would be stored
    // truncated and could then be served for a different host with the same
    // 31-character prefix. Such hosts simply pay the (now parallel) lookup.
    if (ip == 0 || hostname[0] == 0 || strlen(hostname) >= AOIP_NAME_LEN) return;
    for (unsigned i = 0; i < HOST_IP_CACHE_N; i++) {
        if (s_host_cache[i].ip != 0 && strcmp(s_host_cache[i].host, hostname) == 0) {
            s_host_cache[i].ip = ip;
            s_host_cache[i].seen_ms = aoip_now_ms();
            return;
        }
    }
    host_ip_cache_t *v = &s_host_cache[s_host_cache_victim];
    s_host_cache_victim = (s_host_cache_victim + 1u) % HOST_IP_CACHE_N;
    snprintf(v->host, sizeof v->host, "%s", hostname);
    v->ip = ip;
    v->seen_ms = aoip_now_ms();
}

// Searches left uncollected when their gather deadline passed first. They
// are drained at the start of the next cycle — by then their own timeout
// has long elapsed, so their (discarded) results are ready and the handle
// can be freed. Calling mdns_query_async_delete() on a still-running
// search would report INVALID_STATE and leave the handle behind.
// Size covers a full cycle: SRV + TXT for every channel, plus one A query
// per host. Searches parked beyond a full pool are dropped (all 32
// channels on 32 distinct silent hosts and a full pool is a full cycle).
#define UNCOLLECTED_N (2u * UI_CHANNELS)

static mdns_search_once_t *s_uncollected[UNCOLLECTED_N];

static void uncollected_add(mdns_search_once_t *h)
{
    for (unsigned i = 0; i < UNCOLLECTED_N; i++) {
        if (s_uncollected[i] == NULL) {
            s_uncollected[i] = h;
            return;
        }
    }
    // full: the search will expire on its own; the handle is left for its
    // own library to finish (bounded, rare, and only on a fully silent
    // network where the next resolve cycle no longer runs).
}

static void uncollected_drain(void)
{
    for (unsigned i = 0; i < UNCOLLECTED_N; i++) {
        if (!s_uncollected[i]) continue;
        mdns_result_t *r = NULL;
        uint8_t cnt = 0;
        if (mdns_query_async_get_results(s_uncollected[i], 100u, &r, &cnt)) {
            if (r) mdns_query_results_free(r);
            (void)mdns_query_async_delete(s_uncollected[i]);
            s_uncollected[i] = NULL;
        }
        // still running (only plausible right after startup): next cycle
    }
}

// Wait for one search until done or `deadline`; the handle is always
// consumed. Finished: return the results (caller frees with
// mdns_query_results_free; NULL when the answer simply had no records).
// Unfinished: park the handle for the next cycle and return NULL.
static mdns_result_t *gather_search(mdns_search_once_t **qp, uint32_t deadline)
{
    mdns_result_t *r = NULL;
    uint8_t cnt = 0;
    mdns_search_once_t *q = *qp;
    *qp = NULL;
    if (!q) return NULL;
    const int32_t rem = (int32_t)(deadline - aoip_now_ms());
    if (mdns_query_async_get_results(q, rem > 0 ? (uint32_t)rem : 1u, &r, &cnt)) {
        (void)mdns_query_async_delete(q);
        return r;
    }
    uncollected_add(q);
    return NULL;
}

// First IPv4 in an answer, 0 when none.
static uint32_t result_v4(const mdns_result_t *r)
{
    for (const mdns_ip_addr_t *a = r ? r->addr : NULL; a; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) return a->addr.u_addr.ip4.addr;
    }
    return 0;
}

void aoip_mdns_query_chans(const char (*host)[AOIP_NAME_LEN], const char (*chan)[AOIP_NAME_LEN],
                           size_t n, aoip_adv_chan_t *out, bool *ok)
{
    // Caller contract (channels_subscriber.rs): one entry per subscribed
    // channel, n <= UI_CHANNELS. All bookkeeping below is static: subscriber
    // task only, so the mDNS state stays off its 6 KB stack.
    if (n > UI_CHANNELS) n = UI_CHANNELS;
    for (size_t i = 0; i < n; i++) ok[i] = false;
    if (!s_up) return;
    uncollected_drain();

    static mdns_search_once_t *qa[UI_CHANNELS];         // SRV per channel
    static mdns_search_once_t *qt[UI_CHANNELS];         // TXT per channel
    static char inst[UI_CHANNELS][2 * AOIP_NAME_LEN + 2];
    static char shost[UI_CHANNELS][MDNS_NAME_BUF_LEN];  // hostname per SRV answer
    static bool need_ip[UI_CHANNELS];                   // parsed, but address unknown
    static aoip_txt_kv_t kv[32];
    memset(qa, 0, sizeof qa);
    memset(qt, 0, sizeof qt);
    memset(shost, 0, sizeof shost);
    memset(need_ip, 0, sizeof need_ip);

    // Phase 1: ask every channel (SRV + TXT) at once, staggered like the
    // Inferno client, then collect under one hard deadline.
    const uint32_t t0 = aoip_now_ms();
    for (size_t i = 0; i < n; i++) {
        snprintf(inst[i], sizeof inst[i], "%s@%s", chan[i], host[i]);
        qa[i] = mdns_query_async_new(inst[i], CHAN_SERVICE, PROTO_UDP, MDNS_TYPE_SRV,
                                     QUERY_TIMEOUT_MS, 1, NULL);
        qt[i] = mdns_query_async_new(inst[i], CHAN_SERVICE, PROTO_UDP, MDNS_TYPE_TXT,
                                     QUERY_TIMEOUT_MS, 1, NULL);
        vTaskDelay(pdMS_TO_TICKS(8));       // channels_subscriber.rs: 8 ms apart
    }
    const uint32_t srv_deadline = t0 + RESOLVE_SRV_MS;
    for (size_t i = 0; i < n; i++) {
        aoip_adv_chan_t *o = &out[i];
        mdns_result_t *rs = gather_search(&qa[i], srv_deadline);
        mdns_result_t *rt = gather_search(&qt[i], srv_deadline);
        bool good = rs && rt && rs->hostname != NULL;
        if (good) {
            const size_t nv = txt_view(rt, kv, sizeof kv / sizeof kv[0]);
            good = mdns_txt_parse_chan(kv, nv, o);
        }
        if (good) {
            o->port = rs->port;
            o->ip = result_v4(rs);
            if (o->ip) {
                host_cache_store(rs->hostname, o->ip);
            } else if (!host_cache_lookup(rs->hostname, &o->ip)) {
                // SRV answer without an address and no cache hit: join the
                // distinct-host A-record round below.
                need_ip[i] = true;
                snprintf(shost[i], sizeof shost[i], "%s", rs->hostname);
                good = false;
            }
        }
        if (!good) ESP_LOGW(TAG, "could not resolve %s", inst[i]);
        ok[i] = good;
        if (rs) mdns_query_results_free(rs);
        if (rt) mdns_query_results_free(rt);
        // qa[i]/qt[i] were consumed by gather_search (deleted or parked).
    }

    // Phase 2: A records for the distinct hosts that still have no
    // address. All of them are asked at once and gathered under one
    // deadline; success fills every channel of that host.
    static char ah[UI_CHANNELS][MDNS_NAME_BUF_LEN];
    memset(ah, 0, sizeof ah);
    unsigned na = 0;
    for (size_t i = 0; i < n; i++) {
        if (!need_ip[i]) continue;
        unsigned k;
        for (k = 0; k < na; k++) {
            if (strcmp(ah[k], shost[i]) == 0) break;
        }
        if (k == na && k < UI_CHANNELS) {
            snprintf(ah[na], sizeof ah[na], "%s", shost[i]);
            na++;
        }
    }
    if (na) {
        memset(qa, 0, sizeof qa);
        const uint32_t t_a = aoip_now_ms();
        const uint32_t a_deadline = t_a + RESOLVE_A_MS;
        for (unsigned k = 0; k < na; k++) {
            qa[k] = mdns_query_async_new(ah[k], NULL, NULL, MDNS_TYPE_A, QUERY_TIMEOUT_MS, 1, NULL);
            vTaskDelay(pdMS_TO_TICKS(8));
        }
        for (unsigned k = 0; k < na; k++) {
            mdns_result_t *rs = gather_search(&qa[k], a_deadline);
            const uint32_t ip = result_v4(rs);
            if (ip) {
                host_cache_store(ah[k], ip);
                for (size_t i = 0; i < n; i++) {
                    if (need_ip[i] && strcmp(shost[i], ah[k]) == 0) {
                        out[i].ip = ip;
                        ok[i] = true;
                        need_ip[i] = false;
                    }
                }
            }
            if (rs) mdns_query_results_free(rs);
        }
    }
}

bool aoip_mdns_query_bundle(const char *full_name, aoip_adv_bundle_t *o)
{
    if (!s_up) return false;
    mdns_result_t *r = NULL;
    memset(o, 0, sizeof *o);
    if (mdns_query_txt(full_name, BUND_SERVICE, PROTO_UDP, QUERY_TIMEOUT_MS, &r) != ESP_OK || !r) {
        return false;
    }
    aoip_txt_kv_t kv[32];
    const size_t nv = txt_view(r, kv, sizeof kv / sizeof kv[0]);
    const bool good = mdns_txt_parse_bundle(kv, nv, o);
    mdns_query_results_free(r);
    return good;
}
