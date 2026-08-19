#ifndef DEMO_DEVICE_DEVICE_RUNNER_H
#define DEMO_DEVICE_DEVICE_RUNNER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct device_platform device_platform_t;
typedef struct device_host device_host_t;
typedef struct device_runner device_runner_t;

device_runner_t *device_runner_create(const device_platform_t *plat, device_host_t *host);
void device_runner_destroy(device_runner_t *r);
/* thread_plat 仅用于创建工作线程（测试可注入失败平台）；内部运行用 host->plat。 */
int device_runner_start(device_runner_t *r, const device_platform_t *thread_plat);
void device_runner_request_stop(device_runner_t *r);
int device_runner_stop_requested(device_runner_t *r);
int device_runner_join(device_runner_t *r, uint32_t timeout_ms); /* 0=已结束，1=超时 */

/* 测试专用消费者节流钩子：>0 时 runner 每出队一条命令处理前休眠该毫秒数。
 * 生产流程保持 0（不产生任何延迟）；供测试确定性地验证命令队列满语义。 */
extern int device_runner_test_cmd_hold_ms;

#ifdef __cplusplus
}
#endif

#endif
