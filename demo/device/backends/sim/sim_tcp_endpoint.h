/* ============================================================================
 * sim_tcp_endpoint.h - 模拟地址 → loopback 解析（纯函数，无任何状态）。
 *
 * beta v1.1 角色反转语义（设计文档 7.3）：
 *   real_device_mode=1 → 输出严格等于 requested（真实模式不翻译）。
 *   否则（sim 模式）：
 *     命中 PC 热点固定目标（simulated_pc_match=1）
 *       → 127.0.0.1:<pc_loopback_port_host_order>
 *     其余地址 → 不做该翻译，输出严格等于 requested。
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

/* 端序无关 host→network 16 位（与 resolve_endpoint 内部同一语义） */
uint16_t sim_tcp_host_to_net16(uint16_t host_order);

sim_tcp_endpoint_t sim_tcp_resolve_endpoint(
    int real_device_mode,
    const net_addr_t *requested,
    int simulated_pc_match,
    uint16_t simulated_pc_loopback_port_host_order);

#ifdef __cplusplus
}
#endif

#endif
