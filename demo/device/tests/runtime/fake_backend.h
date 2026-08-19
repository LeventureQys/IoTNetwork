#ifndef TEST_FAKE_BACKEND_H
#define TEST_FAKE_BACKEND_H

#include "net_abstraction.h"
#include "device_backend_factory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 测试专用纯 C 假后端：行为通过全局开关控制，NVS 落盘到 options.nvs_file。
 * beta v1.1：扫描返回 pc_ap_ssid、TCP 直连固定 192.168.137.1:5935；
 * 计数旧能力（AP/mDNS/UDP/listen）调用次数供断言从未调用。 */

void fake_backend_reset(void);
void fake_backend_set_auth_fail(int on);    /* sta_connect 返回 AUTH_FAIL(202) */
void fake_backend_set_no_ap(int on);        /* scan 无目标 + sta_connect 返回 NO_AP_FOUND */
void fake_backend_set_similar_ssid(int on); /* scan 返回相似但非精确 SSID（Modu_PCX） */
void fake_backend_set_init_fail(int on);    /* init 返回 DEMO_ERR */
void fake_backend_set_slow_recv(int ms);    /* sock_recv 阻塞 ms 毫秒 */
void fake_backend_set_dhcp_fail(int on);    /* get_ip 返回 0（IP=0 校验失败） */
void fake_backend_set_tcp_fail(int on);     /* tcp_connect 返回 DEMO_ERR */
void fake_backend_set_wifi_drop(int on);    /* get_ip 失败 + current_ssid 空（WiFi 失效） */
void fake_backend_set_ack_ok(int on);       /* sock_recv 返回一次 host_ack ok */
void fake_backend_set_ack_busy(int on);     /* sock_recv 返回一次 host_ack busy */

int  fake_backend_sta_connected(void);
int  fake_backend_sta_disconnect_calls(void);
int  fake_backend_deinit_calls(void);
int  fake_backend_ap_start_calls(void);     /* 应恒为 0 */
int  fake_backend_mdns_calls(void);         /* 应恒为 0 */
int  fake_backend_udp_calls(void);          /* 应恒为 0 */
int  fake_backend_tcp_listen_calls(void);   /* 应恒为 0 */
uint32_t fake_backend_last_connect_ip(void);   /* 网络字节序 */
uint16_t fake_backend_last_connect_port(void); /* 网络字节序 */
const char *fake_backend_last_inject(void);

/* 直接构造 net_ctx（core 测试用） */
const net_backend_t *fake_backend_vtable(void);

/* 工厂符号（runtime/core 测试目标内提供，替代 SS04/SS05 真实实现） */
device_result_t device_sim_backend_create(
    const device_sim_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error);

device_result_t device_linux_backend_create(
    const device_linux_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
