#ifndef DEMO_LINUX_HOTSPOT_CONFIG_H
#define DEMO_LINUX_HOTSPOT_CONFIG_H

#include "linux_hotspot.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct linux_ipv4_cidr {
    uint32_t address;
    uint32_t network;
    uint32_t netmask;
    uint32_t broadcast;
    uint8_t prefix_length;
} linux_ipv4_cidr_t;

typedef struct linux_ipv4_route {
    linux_ipv4_cidr_t destination;
    unsigned int ifindex;
    char interface_name[32];
    unsigned int table;
} linux_ipv4_route_t;

typedef struct linux_hotspot_plan {
    linux_ipv4_cidr_t ap;
    uint32_t dhcp_start;
    uint32_t dhcp_end;
    int candidate_index;
    int used_fallback;
} linux_hotspot_plan_t;

void linux_hotspot_cfg_defaults(linux_hotspot_cfg_t *cfg);
int linux_hotspot_cfg_load(linux_hotspot_cfg_t *cfg, const char *path,
                           char *error, size_t error_capacity);
int linux_ipv4_cidr_parse(const char *address, int prefix_length,
                          linux_ipv4_cidr_t *out);
int linux_hotspot_cfg_validate(const linux_hotspot_cfg_t *cfg,
                               char *error, size_t error_capacity);
int linux_hotspot_select_plan(const linux_hotspot_cfg_t *cfg,
                              const linux_ipv4_route_t *routes,
                              size_t route_count,
                              linux_hotspot_plan_t *out,
                              char *error, size_t error_capacity);
void linux_ipv4_format(uint32_t host_order_ip, char *out, size_t capacity);
void linux_netmask_format(uint32_t host_order_mask, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
