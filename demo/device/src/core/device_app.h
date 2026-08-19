#ifndef DEMO_DEVICE_DEVICE_APP_H
#define DEMO_DEVICE_DEVICE_APP_H

#include "device_priv.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

device_app_t *device_app_create(const device_config_t *params, net_ctx_t *net,
                                const char *device_id, uint32_t seed);
void device_app_destroy(device_app_t *app);
void device_app_run(device_app_t *app);        /* 阻塞事件循环，直到 stop_flag */
int  device_app_run_step(device_app_t *app);   /* 单轮 tick（含 10ms sleep）；返回 0=已请求停止 */
void device_app_request_stop(device_app_t *app);
device_state_t device_app_get_state(const device_app_t *app);
uint32_t device_app_uptime_s(const device_app_t *app);

/* 结构化事件上报（SS06 埋点）：runtime 通过 set_event_sink 注入回调；
 * publish_event 在 ev_fn 为空时安全 no-op。data_json 为可选附加字段 JSON 对象
 * （可为 NULL，表示空对象）。 */
void device_app_set_event_sink(device_app_t *app,
                               void (*fn)(void *user, const char *event,
                                          const char *result, int code,
                                          const char *data_json),
                               void *user);
void device_app_publish_event(device_app_t *app, const char *event,
                              const char *result, int code,
                              const char *data_json);

/* 计算 UTF-8 文本的 SHA-256 并输出十六进制小写（out_hex 容量至少 65，含 NUL）。 */
void device_app_sha256_hex(const char *text, size_t len, char out_hex[65]);

/* device 线程专用：立即发送一条 app_data（会话态 + 限速校验）。
 * 返回 DEMO_OK=已发送；DEMO_ERR=非会话态/限速/发送失败。 */
int device_app_tx_text(device_app_t *app, const char *text);

/* 收包统一入口（provision/discovery/session 复用）：帧解析 + 畸形计数 + cmd 分发 */
void device_app_handle_rx(device_app_t *app, void *sock, int is_business);

/* 内部工具（供子模块使用） */
int  device_send_frame(device_app_t *app, void *sock, cJSON *obj);
int  device_send_frame_raw(device_app_t *app, void *sock, const char *json);
void device_set_state(device_app_t *app, device_state_t s);
void device_sleep_ms(int ms);

#ifdef __cplusplus
}
#endif

#endif
