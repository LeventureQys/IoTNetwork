#ifndef PC_SOCKET_BACKEND_H
#define PC_SOCKET_BACKEND_H

#include "net_abstraction.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PC 自包含 socket 后端：TCP、UDP 组播、非阻塞 socket、时钟与随机数。
 * 不含 WiFi/AP/NVS/SimWorld（设计文档第 7.1 节）。
 * 生命周期：create 后 user 由调用方持有；destroy 幂等。 */
void *pc_socket_backend_create(void);
void pc_socket_backend_destroy(void *user);
const net_backend_t *pc_socket_backend_table(void);

#ifdef __cplusplus
}
#endif

#endif
