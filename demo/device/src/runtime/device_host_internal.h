#ifndef DEMO_DEVICE_DEVICE_HOST_INTERNAL_H
#define DEMO_DEVICE_DEVICE_HOST_INTERNAL_H

#include "device_host.h"
#include "device_config.h"
#include "device_platform.h"
#include "device_backend_factory.h"
#include "device_cmd_queue.h"
#include "device_log_queue.h"
#include "device_events.h"
#include "device_runner.h"
#include "scenario_engine.h"
#include "net_abstraction.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* core 前向声明（不引入 core 私有头） */
typedef struct device_app device_app_t;

struct device_host {
    const device_platform_t *plat;

    /* create 时复制的选项与路径（全部绝对化） */
    device_host_options_t opts;
    char config_path[1024];
    char runtime_dir[1024];
    char nvs_file[1024];
    char sim_catalog_dir[1024];
    char scenario_path[1024];
    char device_id[18];
    int provision_port;
    uint32_t duration_ms;
    uint64_t start_mono_ms;

    device_config_t cfg;

    device_mutex_t *state_mutex;
    device_host_state_t state;

    device_mutex_t *snapshot_mutex;
    device_snapshot_t snapshot;

    device_cmd_queue_t *cmds;
    device_log_queue_t *logs;
    device_events_t *events;

    device_backend_instance_t backend;
    net_ctx_t *net;
    device_app_t *app;

    device_runner_t *runner;

    /* --scenario 执行引擎（仅 sim；可为 NULL） */
    scenario_engine_t *scenario;

    /* 待发 app_data（runner 线程专用） */
    char pending_app_data[APP_DATA_TEXT_MAX];
    size_t pending_app_data_len;
    int pending_app_data_valid;
};

/* 供测试注入平台（线程创建失败等）；默认实现使用 device_platform_default。 */
device_result_t device_host_create_internal(const device_host_options_t *options,
                                            device_host_t **out_host,
                                            device_error_t *error,
                                            const device_platform_t *plat);
device_result_t device_host_start_internal(device_host_t *host, device_error_t *error,
                                           const device_platform_t *thread_plat);

/* runner（device 线程）每轮调用：从核心状态刷新受锁保护的快照。 */
void device_host_snapshot_refresh(device_host_t *host);

/* 事件路由（device 线程/主线程均可调）：写 --events-jsonl 并喂给 scenario 引擎。 */
void device_host_publish_event(device_host_t *host, const char *event,
                               const char *result, int code,
                               const char *data_json);

#ifdef __cplusplus
}
#endif

#endif
