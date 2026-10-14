// Host-test stub for esp_netif.h (type names only; no netif calls on host).
#pragma once

typedef struct esp_netif esp_netif_t;

typedef struct {
    unsigned addr;
} esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip, netmask, gw;
} esp_netif_ip_info_t;

static inline int esp_netif_get_ip_info(esp_netif_t *nif, esp_netif_ip_info_t *info)
{
    (void)nif;
    (void)info;
    return 0;
}
