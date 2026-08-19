#ifndef DEMO_LINUX_SOCKET_H
#define DEMO_LINUX_SOCKET_H

/*
 * 真实 Linux 后端的 POSIX socket / 系统能力层（TCP、UDP 组播、时钟、随机）。
 *
 * 语义与 net_abstraction.h 对齐：
 * - 所有 socket 非阻塞；recv/accept 无数据返回 DEMO_ERR_AGAIN；
 * - tcp_connect 非阻塞连接 + select 超时（timeout_ms，<=0 表示立即返回结果）；
 * - sock 句柄以 (void *)(intptr_t)fd 编码，由 net_sock_close 显式释放；
 * - 仅 Linux 下编译。
 */

#include "net_abstraction.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int linux_socket_tcp_listen(uint16_t port, void **out_sock);
int linux_socket_tcp_accept(void *listen_sock, void **out_conn,
                            net_addr_t *out_peer);
int linux_socket_tcp_connect(const net_addr_t *addr, void **out_sock,
                             int timeout_ms);
int linux_socket_send(void *sock, const uint8_t *buf, int len);
int linux_socket_recv(void *sock, uint8_t *buf, int cap);
void linux_socket_close(void *sock);
int linux_socket_udp_mcast_join(const char *group, uint16_t port, void **out_sock);
int linux_socket_udp_send(const char *group, uint16_t port, const uint8_t *buf,
                          int len);
int linux_socket_udp_recv(void *sock, uint8_t *buf, int cap, net_addr_t *out_from);
uint64_t linux_socket_time_ms(void);
uint32_t linux_socket_random(void);

#ifdef __cplusplus
}
#endif

#endif
