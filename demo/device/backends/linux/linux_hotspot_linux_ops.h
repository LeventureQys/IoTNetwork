#ifndef DEMO_LINUX_HOTSPOT_LINUX_OPS_H
#define DEMO_LINUX_HOTSPOT_LINUX_OPS_H

#include "linux_hotspot_config.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct linux_hotspot_ops {
    int (*run_argv)(const char *const argv[]);
    int (*collect_routes)(linux_ipv4_route_t *routes, size_t capacity,
                          size_t *count);
    int (*interface_has_ipv4)(const char *interface_name,
                              const linux_ipv4_cidr_t *expected);
    int (*pidfile_read)(const char *path, pid_t *pid);
    int (*pid_is_expected)(pid_t pid, const char *expected_executable);
    int (*pid_has_udp_listener)(pid_t pid, uint16_t port,
                                uint32_t expected_address);
    uint64_t (*monotonic_ms)(void);
    void (*sleep_ms)(unsigned int milliseconds);
    int (*interface_exists)(const char *interface_name);
    int (*interface_is_ap)(const char *interface_name);
    int (*is_root)(void);
    int (*binary_available)(const char *executable);
    int (*sta_channel)(const char *interface_name);
    int (*ip_forward_enabled)(void);
} linux_hotspot_ops_t;

/* 生产 ops 表（仅 Linux 有实现；非 Linux 返回 NULL）。 */
const linux_hotspot_ops_t *linux_hotspot_default_ops(void);

#ifdef __cplusplus
}
#endif

#endif
