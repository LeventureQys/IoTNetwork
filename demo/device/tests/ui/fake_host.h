#ifndef DEMO_DEVICE_TESTS_UI_FAKE_HOST_H
#define DEMO_DEVICE_TESTS_UI_FAKE_HOST_H

#include "device_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* facade fake：不链接真实 device_host.c；由测试提供同签名实现。
 * 通过 fake_host_xxx 控制快照/日志/返回值，验证 DeviceWindow 行为。 */

void fake_host_init(void);
/* 返回一个非空但内容无关的伪 device_host_t*（桩函数全部忽略指针内容） */
device_host_t *fake_host_instance(void);
void fake_host_set_snapshot(const device_snapshot_t *snap);
void fake_host_set_drain_result(device_result_t rc);
void fake_host_push_log(const device_log_record_t *rec);
void fake_host_set_send_app_data_result(device_result_t rc);
void fake_host_set_inject_result(device_result_t rc);

/* 观察量 */
int fake_host_stop_requested(void);
int fake_host_send_calls(void);
int fake_host_inject_calls(void);
const char *fake_host_last_app_data(void);
size_t fake_host_last_app_data_len(void);
const char *fake_host_last_inject_action(void);
const char *fake_host_last_inject_argument(void);

#ifdef __cplusplus
}
#endif

#endif
