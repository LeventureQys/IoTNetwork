#ifndef DEMO_DEVICE_SCENARIO_ENGINE_H
#define DEMO_DEVICE_SCENARIO_ENGINE_H

#include "device_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 设备侧 scenario 执行引擎（SS06，仅 sim 后端）。
 *
 * 契约（设计文档第 11 节）：
 * - 文件 schema 固定为 { schema:1, actions:[...] }；
 * - action 一次一结果：每个 target=device 的 action 恰好产生一条同 action_id
 *   的 scenario_result 事件（status=ok|rejected|failed）；
 * - after_event 门控：ready|ap_ready|session_online|session_offline|null；
 * - delay_ms 范围 0~60000；
 * - auto_provision.args 需要 ssid/password；send_app_data.args 需要 text；
 *   inject_fault.args 需要 name、可选 argument_json；request_stop.args 必须为空对象；
 * - target=pc 的 action 忽略（不产生 scenario_result）；
 * - target=当前进程但 action/args/schema 非法时创建失败（启动失败）。
 *
 * 引擎运行于 device 线程（runner 每轮 tick），事件流经 host 的事件回调注入。 */

typedef struct scenario_engine scenario_engine_t;
typedef struct device_host device_host_t;

/* 解析并校验 scenario 文件；非法时返回 NULL 并填充 error（DEVICE_ERR_CONFIG_INVALID）。 */
scenario_engine_t *scenario_engine_create(const char *scenario_path,
                                          device_host_t *host,
                                          device_error_t *error);

void scenario_engine_destroy(scenario_engine_t *eng);

/* 事件流注入（host 事件回调转发）：用于 after_event 门控。 */
void scenario_engine_on_event(scenario_engine_t *eng, const char *event,
                              const char *result, const char *data_json);

/* runner 每轮调用：推进门控/延时并执行到期 action（一次一结果）。 */
void scenario_engine_tick(scenario_engine_t *eng, device_host_t *host);

#ifdef __cplusplus
}
#endif

#endif
