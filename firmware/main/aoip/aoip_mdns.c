// SPDX-License-Identifier: GPL-3.0-or-later
//
// mDNS advertisement and lookups (espressif/mdns). Translated from Inferno
// device_server/mdns_server.rs (what we publish) and mdns_client.rs (how we
// resolve transmitters' channels and multicast bundles).
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
// Lookups (mdns_client.rs)
// ---------------------------------------------------------------------------
static const char *txt_get(const mdns_result_t *r, const char *key, char *buf, size_t cap)
{
    for (size_t i = 0; r && i < r->txt_count; i++) {
        if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) {
            size_t n = r->txt_value_len ? r->txt_value_len[i] : 0;
            if (!r->txt[i].value) n = 0;
            if (n >= cap) n = cap - 1;
            if (n) memcpy(buf, r->txt[i].value, n);
            buf[n] = 0;
            return buf;
        }
    }
    return NULL;
}

// parse_int_from_dict: "0x" prefix = hex, else decimal
static bool txt_int(const mdns_result_t *r, const char *key, uint32_t *out)
{
    char buf[24];
    const char *s = txt_get(r, key, buf, sizeof buf);
    if (!s || !s[0]) return false;
    char *end;
    unsigned long v = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? strtoul(s + 2, &end, 16)
                                                                     : strtoul(s, &end, 10);
    if (*end) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_ipv4(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    uint8_t *p = (uint8_t *)out;
    p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = (uint8_t)c; p[3] = (uint8_t)d;
    return true;
}

static bool parse_chan_txt(const mdns_result_t *r, aoip_adv_chan_t *o)
{
    uint32_t v;
    if (!txt_int(r, "nchan", &v)) return false;
    o->nchan = (uint16_t)v;
    if (!txt_int(r, "id", &v)) return false;
    o->tx_channel_id = (uint16_t)v;
    if (!txt_int(r, "enc", &v) && !txt_int(r, "en", &v)) return false;
    o->bits = v;
    if (!txt_int(r, "dbcp1", &v)) return false;
    o->dbcp1 = (uint16_t)v;
    char buf[24];
    const char *fpp = txt_get(r, "fpp", buf, sizeof buf);
    unsigned fmax, fmin;
    if (!fpp || sscanf(fpp, "%u,%u", &fmax, &fmin) != 2) return false;
    o->fpp_max = (uint16_t)fmax;
    o->fpp_min = (uint16_t)fmin;
    if (!txt_int(r, "latency_ns", &v)) return false;
    o->min_latency_ns = v;
    o->has_bundle = false;
    for (size_t i = 0; i < r->txt_count; i++) {
        const char *k = r->txt[i].key;
        if (!k || strncmp(k, "b.", 2) != 0) continue;
        char *end;
        unsigned long bid = strtoul(k + 2, &end, 10);
        if (*end || k[2] == 0) break;               // "Unable to parse multicast bundle key"
        char vb[12];
        if (!txt_get(r, k, vb, sizeof vb)) break;
        unsigned long chb = strtoul(vb, &end, 10);
        if (*end || chb == 0) break;
        o->has_bundle = true;
        o->bundle_id = (uint16_t)bid;
        o->channel_in_bundle = (uint16_t)(chb - 1u);
        break;
    }
    return true;
}

#define QUERY_TIMEOUT_MS 3000u
#define QUERY_BATCH      8u

void aoip_mdns_query_chans(const char (*host)[AOIP_NAME_LEN], const char (*chan)[AOIP_NAME_LEN],
                           size_t n, aoip_adv_chan_t *out, bool *ok)
{
    for (size_t i = 0; i < n; i++) ok[i] = false;
    if (!s_up) return;
    for (size_t base = 0; base < n; base += QUERY_BATCH) {
        size_t m = n - base < QUERY_BATCH ? n - base : QUERY_BATCH;
        mdns_search_once_t *srv[QUERY_BATCH] = { 0 }, *txt[QUERY_BATCH] = { 0 };
        char inst[QUERY_BATCH][2 * AOIP_NAME_LEN + 2];
        for (size_t j = 0; j < m; j++) {
            snprintf(inst[j], sizeof inst[j], "%s@%s", chan[base + j], host[base + j]);
            srv[j] = mdns_query_async_new(inst[j], CHAN_SERVICE, PROTO_UDP, MDNS_TYPE_SRV,
                                          QUERY_TIMEOUT_MS, 1, NULL);
            txt[j] = mdns_query_async_new(inst[j], CHAN_SERVICE, PROTO_UDP, MDNS_TYPE_TXT,
                                          QUERY_TIMEOUT_MS, 1, NULL);
            vTaskDelay(pdMS_TO_TICKS(8));   // channels_subscriber.rs: 8 ms apart
        }
        for (size_t j = 0; j < m; j++) {
            aoip_adv_chan_t *o = &out[base + j];
            memset(o, 0, sizeof *o);
            mdns_result_t *rs = NULL, *rt = NULL;
            uint8_t ns = 0, nt = 0;
            if (srv[j]) mdns_query_async_get_results(srv[j], QUERY_TIMEOUT_MS + 500u, &rs, &ns);
            if (txt[j]) mdns_query_async_get_results(txt[j], QUERY_TIMEOUT_MS + 500u, &rt, &nt);
            bool good = rs && rt && rs->hostname && parse_chan_txt(rt, o);
            if (good) {
                o->port = rs->port;
                o->ip = 0;
                for (mdns_ip_addr_t *a = rs->addr; a; a = a->next) {
                    if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                        o->ip = a->addr.u_addr.ip4.addr;
                        break;
                    }
                }
                if (!o->ip) {
                    esp_ip4_addr_t a4 = { 0 };
                    if (mdns_query_a(rs->hostname, QUERY_TIMEOUT_MS, &a4) == ESP_OK) o->ip = a4.addr;
                }
                good = o->ip != 0;
            }
            ok[base + j] = good;
            if (!good) ESP_LOGW(TAG, "could not resolve %s", inst[j]);
            if (rs) mdns_query_results_free(rs);
            if (rt) mdns_query_results_free(rt);
            if (srv[j]) mdns_query_async_delete(srv[j]);
            if (txt[j]) mdns_query_async_delete(txt[j]);
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
    bool good = false;
    char buf[24];
    uint32_t v;
    do {
        const char *a = txt_get(r, "a.0", buf, sizeof buf);
        if (!a || !parse_ipv4(a, &o->ip)) break;
        if (!txt_int(r, "p.0", &v)) break;
        o->port = (uint16_t)v;
        if (!txt_int(r, "nchan", &v)) break;
        o->nchan = (uint16_t)v;
        if (!txt_int(r, "id", &v)) break;
        o->bundle_id = (uint16_t)v;
        if (!txt_int(r, "enc", &v) && !txt_int(r, "en", &v)) break;
        o->bits = v;
        if (!txt_int(r, "fpp", &v)) break;
        o->fpp = (uint16_t)v;
        if (!txt_int(r, "latency_ns", &v)) break;
        o->min_latency_ns = v;
        good = true;
    } while (0);
    mdns_query_results_free(r);
    return good;
}
