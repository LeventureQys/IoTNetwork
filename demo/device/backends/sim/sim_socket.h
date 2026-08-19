/* ============================================================================
 * sim_socket.h - TCP/UDP/组播与平台错误映射（纯 C11，实例无关函数）。
 *
 * 语义冻结于旧 net_sim/sim_backend.cpp 的 socket 部分：
 *   - TCP/UDP socket 全部非阻塞；recv/accept 无数据返回 DEMO_ERR_AGAIN。
 *   - tcp_connect 为阻塞式连接 + select 超时（默认 3000ms）；
 *     连接被拒（ECONNREFUSED）映射为 DEMO_ERR_TIMEOUT（可重试语义）。
 *   - tcp_listen 绑定 127.0.0.1（模拟模式，无 Linux 真实热点分支）。
 *   - udp_recv 支持 drain_until_block（组播屏蔽时丢弃全部收包）。
 * 平台错误码（WSAGetLastError / errno）→ DEMO_* 的映射全部收敛于本模块。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_SOCKET_H
#define DEMO_DEVICE_SIM_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "net_abstraction.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WSA 生命周期：与实例创建/销毁配对（WSAStartup 进程级引用计数）。POSIX 恒 OK/空操作。 */
int sim_socket_startup(void);
void sim_socket_cleanup(void);

int sim_socket_tcp_listen(uint16_t port, void **sock);
int sim_socket_tcp_accept(void *listen_sock, void **conn, net_addr_t *peer);
int sim_socket_tcp_connect(const net_addr_t *addr, void **sock, int timeout_ms);
int sim_socket_send(void *sock, const uint8_t *buf, int len);
int sim_socket_recv(void *sock, uint8_t *buf, int cap);
void sim_socket_close(void *sock);

int sim_socket_udp_mcast_join(const char *group, uint16_t port, void **sock);
int sim_socket_udp_send(const char *group, uint16_t port, const uint8_t *buf, int len);
int sim_socket_udp_recv(void *sock, uint8_t *buf, int cap, net_addr_t *from, int drain_until_block);

#ifdef __cplusplus
}
#endif

#endif
