#ifndef TEST_FAKE_BACKEND_H
#define TEST_FAKE_BACKEND_H

#include "net_abstraction.h"
#include "device_backend_factory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 测试专用纯 C 假后端：行为通过全局开关控制，NVS 落盘到 options.nvs_file。 */

void fake_backend_reset(void);
void fake_backend_set_auth_fail(int on);   /* wifi_sta_connect 返回 AUTH_FAIL(202) */
void fake_backend_set_no_ap(int on);       /* wifi_sta_connect 返回 NO_AP_FOUND(201) */
void fake_backend_set_announce(int on);    /* mdns_resolve 返回可用上位机 127.0.0.1:5935 */
void fake_backend_set_init_fail(int on);   /* init 返回 DEMO_ERR */
void fake_backend_set_slow_recv(int ms);   /* sock_recv 阻塞 ms 毫秒（模拟慢对端） */
int  fake_backend_ap_started(void);
int  fake_backend_sta_connected(void);
int  fake_backend_deinit_calls(void);
const char *fake_backend_last_inject(void);

/* 直接构造 net_ctx（core 测试用） */
const net_backend_t *fake_backend_vtable(void);

/* 工厂符号（runtime/core 测试目标内提供，替代 SS04/SS05 未落地实现） */
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
