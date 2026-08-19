/* ============================================================================
 * sim_tcp_endpoint.c - 模拟地址 → loopback 解析（纯函数，无全局状态）。
 *
 * beta v1.1 角色反转语义：仅 PC 热点固定目标命中时翻译到 loopback，
 * 其余地址不翻译（保持请求端点）。
 * ========================================================================== */
#include "sim_tcp_endpoint.h"

#include <stddef.h>

/* 按网络字节序字节序列组装 uint32 值（端序无关） */
static uint32_t net_ipv4(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    uint32_t value = 0;
    uint8_t *bytes = (uint8_t *)&value;
    bytes[0] = b0;
    bytes[1] = b1;
    bytes[2] = b2;
    bytes[3] = b3;
    return value;
}

/* 127.0.0.1 的网络字节序值 */
static uint32_t loopback_ip(void)
{
    return net_ipv4(127, 0, 0, 1);
}

uint16_t sim_tcp_host_to_net16(uint16_t value)
{
    union {
        uint16_t u16;
        uint8_t bytes[2];
    } probe;
    probe.u16 = 1;
    if (probe.bytes[0] == 1)
        return (uint16_t)((value << 8) | (value >> 8));
    return value;
}

sim_tcp_endpoint_t sim_tcp_resolve_endpoint(
    int real_device_mode,
    const net_addr_t *requested,
    int simulated_pc_match,
    uint16_t simulated_pc_loopback_port_host_order)
{
    sim_tcp_endpoint_t out;
    out.ip = 0;
    out.port = 0;
    if (requested == NULL)
        return out;

    if (real_device_mode) {
        /* 真实模式：不做任何翻译。 */
        out.ip = requested->ip;
        out.port = requested->port;
        return out;
    }

    if (simulated_pc_match) {
        out.ip = loopback_ip();
        out.port = sim_tcp_host_to_net16(simulated_pc_loopback_port_host_order);
    } else {
        /* 其余地址不做翻译。 */
        out.ip = requested->ip;
        out.port = requested->port;
    }
    return out;
}
