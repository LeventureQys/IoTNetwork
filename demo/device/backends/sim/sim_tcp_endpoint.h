/* ============================================================================
 * sim_tcp_endpoint.h - 模拟地址 → loopback 解析（纯函数，无任何状态）。
 *
 * 语义冻结于旧 net_sim/sim_tcp_endpoint.cpp（设计文档第 8.5 节）：
 *   real_device_mode=1 → 输出严格等于 requested（真实模式不翻译）；
 *   否则按模拟语义翻译：
 *     模拟 AP 命中            → 127.0.0.1:<simulated_ap_real_port_host_order>
 *     请求等于模拟 Host IP    → 127.0.0.1:<simulated_host_port_host_order>
 *     其余地址                → 127.0.0.1:<requested.port>（既有兼容行为）
 * ip/port 均为网络字节序；requested 为 NULL 时输出零端点。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_TCP_ENDPOINT_H
#define DEMO_DEVICE_SIM_TCP_ENDPOINT_H

#include <stdint.h>
#include "net_abstraction.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_tcp_endpoint {
    uint32_t ip;    /* 网络字节序 */
    uint16_t port;  /* 网络字节序 */
} sim_tcp_endpoint_t;

sim_tcp_endpoint_t sim_tcp_resolve_endpoint(
    int real_device_mode,
    const net_addr_t *requested,
    int simulated_ap_match,
    uint16_t simulated_ap_real_port_host_order,
    uint32_t simulated_host_ip_network_order,
    uint16_t simulated_host_port_host_order);

#ifdef __cplusplus
}
#endif

#endif
