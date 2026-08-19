#ifndef PC_NET_ABSTRACTION_H
#define PC_NET_ABSTRACTION_H
#include <stdint.h>
#include <stddef.h>
#include "common.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct net_ctx net_ctx_t;
typedef struct net_backend net_backend_t;

/* ip/port 均为网络字节序 */
typedef struct net_addr { uint32_t ip; uint16_t port; } net_addr_t;
typedef struct net_ap_info { char ssid[33]; int rssi; int band_2g; char bssid[18]; } net_ap_info_t;
typedef struct net_mdns_service { char instance[64]; char type[64]; net_addr_t addr; char txt[128]; } net_mdns_service_t;

/* PC 主动热点状态（设计文档 7.1）：started=1 表示热点已运行。 */
typedef struct net_ap_status {
    int started;
    char ssid[33];
    char ipv4[16];
    int prefix_length;
} net_ap_status_t;

/* 后端 vtable：所有函数由后端实现；便捷封装见下方 net_xxx 系列 */
typedef struct net_backend {
    /* 生命周期 */
    int  (*init)(void *user, const char *config_path);
    void (*deinit)(void *user);
    /* WiFi 介质 */
    int  (*wifi_scan)(void *user, net_ap_info_t *aps, int *count);             /* count 入=容量 出=数量 */
    int  (*wifi_sta_connect)(void *user, const char *ssid, const char *pass, wifi_reason_t *reason);
    int  (*wifi_sta_disconnect)(void *user);
    int  (*wifi_ap_start)(void *user, const char *ssid, const char *pass, const char *pin);
    int  (*wifi_ap_stop)(void *user);
    int  (*wifi_ap_status)(void *user, net_ap_status_t *status);                /* 热点未运行返回 DEMO_ERR */
    int  (*wifi_ap_configure_ipv4)(void *user, const char *ipv4, int prefix_length);
    int  (*wifi_get_rssi)(void *user, int *rssi);                               /* dBm */
    int  (*wifi_get_ip)(void *user, uint32_t *ip);                              /* 当前 STA/AP 虚拟 IP，网络字节序 */
    int  (*wifi_get_current_ssid)(void *user, char *ssid, int capacity);         /* 当前 STA 实际关联 SSID */
    int  (*wifi_get_gateway)(void *user, uint32_t *ip);                          /* 当前 STA 接口 IPv4 网关，网络字节序 */
    /* TCP（非阻塞：recv/accept 无数据返回 DEMO_ERR_AGAIN；connect 阻塞至多 timeout_ms） */
    int  (*tcp_listen)(void *user, uint16_t port, void **sock);
    int  (*tcp_accept)(void *user, void *listen, void **conn, net_addr_t *peer);
    int  (*tcp_connect)(void *user, const net_addr_t *addr, void **sock, int timeout_ms);
    int  (*sock_send)(void *user, void *sock, const uint8_t *buf, int len);
    int  (*sock_recv)(void *user, void *sock, uint8_t *buf, int cap);
    void (*sock_close)(void *user, void *sock);
    /* UDP 组播 */
    int  (*udp_mcast_join)(void *user, const char *group, uint16_t port, void **sock);
    int  (*udp_send)(void *user, const char *group, uint16_t port, const uint8_t *buf, int len);
    int  (*udp_recv)(void *user, void *sock, uint8_t *buf, int cap, net_addr_t *from);
    /* mDNS */
    int  (*mdns_register)(void *user, const net_mdns_service_t *svc);
    int  (*mdns_unregister)(void *user, const char *type);
    int  (*mdns_resolve)(void *user, const char *type, net_mdns_service_t *out, int timeout_ms);
    /* NVS（blob 键值；len 入=容量 出=实际长度；键不存在返回 DEMO_ERR 且 *len=0） */
    int  (*nvs_get)(void *user, const char *key, uint8_t *buf, int *len);
    int  (*nvs_set)(void *user, const char *key, const uint8_t *buf, int len);
    int  (*nvs_erase)(void *user, const char *key);
    /* 系统 */
    uint64_t (*time_ms)(void *user);
    uint32_t (*random)(void *user);
    /* 注入：仅 sim 后端实现；其余后端返回 DEMO_ERR */
    int  (*inject)(void *user, const char *action, const char *arg_json);
} net_backend_t;

/* 便捷封装：转发到 ctx->backend；ctx/backend/函数指针为 NULL 时安全返回（DEMO_ERR 或默认值）。
 * net_ctx_create 契约（beta v1.0.5 设计文档第 9 节）：
 *   - 进入时先置 *out_ctx=NULL；
 *   - backend 或 out_ctx 为空返回 DEMO_ERR_INVAL；
 *   - init 为空视为无需初始化；init 非空且失败时释放 ctx 并返回原错误；
 *   - 仅 init 成功后标记 initialized；destroy 只对已初始化后端调用 deinit；
 *   - backend_user 的所有权归具体 backend wrapper，不由 net_ctx_destroy 释放。 */
int  net_ctx_create(const net_backend_t *be, void *user, const char *config_path,
                    net_ctx_t **out_ctx);
void net_ctx_destroy(net_ctx_t *ctx);

int  net_wifi_scan(net_ctx_t *c, net_ap_info_t *aps, int *count);
int  net_wifi_sta_connect(net_ctx_t *c, const char *ssid, const char *pass, wifi_reason_t *reason);
int  net_wifi_sta_disconnect(net_ctx_t *c);
int  net_wifi_ap_start(net_ctx_t *c, const char *ssid, const char *pass, const char *pin);
int  net_wifi_ap_stop(net_ctx_t *c);
int  net_wifi_ap_status(net_ctx_t *c, net_ap_status_t *status);
int  net_wifi_ap_configure_ipv4(net_ctx_t *c, const char *ipv4, int prefix_length);
int  net_wifi_get_rssi(net_ctx_t *c, int *rssi);
int  net_wifi_get_ip(net_ctx_t *c, uint32_t *ip);
int  net_wifi_get_current_ssid(net_ctx_t *c, char *ssid, int capacity);
int  net_wifi_get_gateway(net_ctx_t *c, uint32_t *ip);
int  net_tcp_listen(net_ctx_t *c, uint16_t port, void **sock);
int  net_tcp_accept(net_ctx_t *c, void *listen, void **conn, net_addr_t *peer);
int  net_tcp_connect(net_ctx_t *c, const net_addr_t *addr, void **sock, int timeout_ms);
int  net_sock_send(net_ctx_t *c, void *sock, const uint8_t *buf, int len);
int  net_sock_recv(net_ctx_t *c, void *sock, uint8_t *buf, int cap);
void net_sock_close(net_ctx_t *c, void *sock);
int  net_udp_mcast_join(net_ctx_t *c, const char *group, uint16_t port, void **sock);
int  net_udp_send(net_ctx_t *c, const char *group, uint16_t port, const uint8_t *buf, int len);
int  net_udp_recv(net_ctx_t *c, void *sock, uint8_t *buf, int cap, net_addr_t *from);
int  net_mdns_register(net_ctx_t *c, const net_mdns_service_t *svc);
int  net_mdns_unregister(net_ctx_t *c, const char *type);
int  net_mdns_resolve(net_ctx_t *c, const char *type, net_mdns_service_t *out, int timeout_ms);
int  net_nvs_get(net_ctx_t *c, const char *key, uint8_t *buf, int *len);
int  net_nvs_set(net_ctx_t *c, const char *key, const uint8_t *buf, int len);
int  net_nvs_erase(net_ctx_t *c, const char *key);
uint64_t net_time_ms(net_ctx_t *c);
uint32_t net_random(net_ctx_t *c);
int  net_inject(net_ctx_t *c, const char *action, const char *arg_json);

#ifdef __cplusplus
}
#endif

#endif
