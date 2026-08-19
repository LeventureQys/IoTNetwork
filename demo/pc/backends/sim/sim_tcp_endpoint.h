#ifndef DEMO_SIM_TCP_ENDPOINT_H
#define DEMO_SIM_TCP_ENDPOINT_H

#include <stdint.h>
#include "net_abstraction.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 设备 TCP 连接目标解析（纯函数，无 socket/WiFi/全局状态依赖）。
 *
 * real_device_mode=1（Linux 真实设备模式）时输出严格等于 requested；
 * 否则按模拟语义翻译：
 *   模拟 AP 命中 -> 127.0.0.1:<ap_real_port>
 *   请求等于模拟 Host IP -> 127.0.0.1:<host_tcp_port>
 *   其余地址保持既有模拟兼容行为 -> 127.0.0.1:<requested.port>
 *
 * ip/port 均为网络字节序（与 net_addr_t 契约一致）。
 * requested 为 NULL 时输出零端点。 */
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
