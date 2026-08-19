#include "device_host.h"
#include "device_host_internal.h"
#include "device_config.h"
#include "device_path.h"
#include "device_backend_factory.h"
#include "device_app.h"
#include "net_abstraction.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* ---------------- 工具 ---------------- */

static void err_set(device_error_t *error, device_result_t code, int platform_code,
                    const char *operation, const char *message)
{
    if (error == NULL)
        return;
    error->code = code;
    error->platform_code = platform_code;
    if (operation != NULL)
        snprintf(error->operation, sizeof(error->operation), "%s", operation);
    if (message != NULL)
        snprintf(error->message, sizeof(error->message), "%s", message);
}

static device_result_t map_demo_err(int rc)
{
    switch (rc) {
    case DEMO_OK:          return DEVICE_OK;
    case DEMO_ERR_NOMEM:   return DEVICE_ERR_NO_MEMORY;
    case DEMO_ERR_INVAL:   return DEVICE_ERR_INVALID_ARGUMENT;
    case DEMO_ERR_TIMEOUT: return DEVICE_ERR_TIMEOUT;
    default:               return DEVICE_ERR_INTERNAL;
    }
}

static void host_set_last_error(device_host_t *host, const char *message)
{
    if (host == NULL || host->snapshot_mutex == NULL)
        return;
    host->plat->mutex_lock(host->snapshot_mutex);
    if (message != NULL)
        snprintf(host->snapshot.last_error, sizeof(host->snapshot.last_error), "%s",
                 message);
    host->plat->mutex_unlock(host->snapshot_mutex);
}

static void host_snapshot_update(device_host_t *host)
{
    device_app_t *app = host->app;
    host->plat->mutex_lock(host->state_mutex);
    device_host_state_t st = host->state;
    host->plat->mutex_unlock(host->state_mutex);

    host->plat->mutex_lock(host->snapshot_mutex);
    host->snapshot.host_state = st;
    if (app != NULL) {
        host->snapshot.device_state = (int)app->state;
        host->snapshot.rssi_dbm = app->snap_rssi;
        host->snapshot.uptime_seconds = app->snap_uptime_s;
        host->snapshot.session_online =
            (app->state == DEV_STATE_SESSION && app->sess_sock != NULL) ? 1 : 0;
        snprintf(host->snapshot.ap_ssid, sizeof(host->snapshot.ap_ssid), "%s",
                 app->ap_ssid);
        snprintf(host->snapshot.ap_password, sizeof(host->snapshot.ap_password), "%s",
                 app->ap_password);
        snprintf(host->snapshot.provision_pin, sizeof(host->snapshot.provision_pin), "%s",
                 app->ap_pin);
    }
    host->plat->mutex_unlock(host->snapshot_mutex);
}

void device_host_snapshot_refresh(device_host_t *host)
{
    if (host != NULL)
        host_snapshot_update(host);
}

/* ---------------- 事件路由（core 埋点 / scenario 结果 / 生命周期事件） ---------------- */

static void host_core_event_sink(void *user, const char *event, const char *result,
                                 int code, const char *data_json)
{
    device_host_publish_event((device_host_t *)user, event, result, code, data_json);
}

void device_host_publish_event(device_host_t *host, const char *event,
                               const char *result, int code,
                               const char *data_json)
{
    if (host == NULL || event == NULL || result == NULL)
        return;
    if (host->events != NULL)
        device_events_write(host->events, event, result, code, data_json);
    if (host->scenario != NULL)
        scenario_engine_on_event(host->scenario, event, result, data_json);
}

/* ---------------- 日志 sink（C 全局单订阅者，不注册 Qt 对象） ---------------- */

static device_host_t *g_sink_host = NULL;

static void host_log_sink(int level, const char *module, const char *msg)
{
    device_host_t *host = g_sink_host;
    if (host == NULL || host->logs == NULL || host->state_mutex == NULL)
        return;
    host->plat->mutex_lock(host->state_mutex);
    device_host_state_t st = host->state;
    host->plat->mutex_unlock(host->state_mutex);
    if (st == DEVICE_HOST_STOPPED)
        return; /* join 成功后不再写日志 */
    device_log_queue_push(host->logs, level, module, msg);
}

static const char *backend_name(device_backend_kind_t kind)
{
    switch (kind) {
    case DEVICE_BACKEND_SIM:     return "sim";
    case DEVICE_BACKEND_LINUX:   return "linux";
    case DEVICE_BACKEND_ESP32C2: return "esp32c2";
    default:                     return "unknown";
    }
}

static long device_process_id(void)
{
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

/* ---------------- create ---------------- */

static int copy_opt_string(const char *src, char *dst, size_t cap)
{
    if (src == NULL || src[0] == '\0') {
        dst[0] = '\0';
        return DEMO_OK;
    }
    if (device_path_absolute(src, dst, cap) != DEMO_OK)
        return DEMO_ERR;
    return DEMO_OK;
}

device_result_t device_host_create(const device_host_options_t *options,
                                   device_host_t **out_host, device_error_t *error)
{
    return device_host_create_internal(options, out_host, error, &device_platform_default);
}

device_result_t device_host_create_internal(const device_host_options_t *options,
                                            device_host_t **out_host,
                                            device_error_t *error,
                                            const device_platform_t *plat)
{
    device_result_t fail_rc = DEVICE_ERR_INTERNAL;
    if (out_host != NULL)
        *out_host = NULL;
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (options == NULL || out_host == NULL) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "create", "options/out_host 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (plat == NULL)
        plat = &device_platform_default;

    if (options->device_index > 15) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "create",
                "设备索引必须在 0 到 15 之间");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (options->backend < DEVICE_BACKEND_SIM || options->backend > DEVICE_BACKEND_ESP32C2) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "create", "未知后端类型");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (options->scenario_path != NULL && options->scenario_path[0] != '\0' &&
        options->backend != DEVICE_BACKEND_SIM) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "create",
                "--scenario 只允许 sim 后端");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }

    device_host_t *host = (device_host_t *)calloc(1, sizeof(device_host_t));
    if (host == NULL) {
        err_set(error, DEVICE_ERR_NO_MEMORY, 0, "create", "分配 host 失败");
        return DEVICE_ERR_NO_MEMORY;
    }
    host->plat = plat;
    host->opts = *options;
    host->duration_ms = options->duration_seconds * 1000u;

    /* 1. 路径规范化（CLI 相对路径按启动 CWD 解析） */
    if (copy_opt_string(options->config_path, host->config_path,
                        sizeof(host->config_path)) != DEMO_OK) {
        fail_rc = DEVICE_ERR_IO;
        err_set(error, fail_rc, 0, "create", "config_path 路径规范化失败");
        goto fail;
    }

    /* 2. 配置加载（缺失 → 内置默认 + 警告；配置内相对路径按配置目录解析） */
    {
        char config_dir[1024] = {0};
        int missing = 0;
        if (device_config_load(&host->cfg, host->config_path, config_dir,
                               sizeof(config_dir), &missing) != DEMO_OK) {
            fail_rc = DEVICE_ERR_CONFIG_INVALID;
            err_set(error, fail_rc, 0, "create", "配置文件非法");
            goto fail;
        }
    }

    /* 3. 运行目录（--runtime-dir 覆盖配置 nvs_dir） */
    {
        char runtime_dir[1024] = {0};
        if (options->runtime_dir != NULL && options->runtime_dir[0] != '\0') {
            if (copy_opt_string(options->runtime_dir, runtime_dir,
                                sizeof(runtime_dir)) != DEMO_OK) {
                fail_rc = DEVICE_ERR_IO;
                err_set(error, fail_rc, 0, "create", "runtime_dir 路径规范化失败");
                goto fail;
            }
        } else {
            snprintf(runtime_dir, sizeof(runtime_dir), "%s", host->cfg.nvs_dir);
            /* 配置缺失时 nvs_dir 可能仍是相对默认值：按 CWD 绝对化，杜绝隐式相对路径 */
            if (!device_path_is_absolute(runtime_dir)) {
                if (device_path_absolute(runtime_dir, runtime_dir,
                                         sizeof(runtime_dir)) != DEMO_OK) {
                    fail_rc = DEVICE_ERR_IO;
                    err_set(error, fail_rc, 0, "create", "运行目录路径规范化失败");
                    goto fail;
                }
            }
        }
        snprintf(host->runtime_dir, sizeof(host->runtime_dir), "%s", runtime_dir);
        if (device_path_mkdirs(host->runtime_dir) != DEMO_OK) {
            fail_rc = DEVICE_ERR_IO;
            err_set(error, fail_rc, 0, "create", "创建运行目录失败");
            goto fail;
        }
    }

    /* 4. fresh：只删除解析出的本设备 NVS 文件 */
    snprintf(host->nvs_file, sizeof(host->nvs_file), "%s/dev%u.nvs.json",
             host->runtime_dir, options->device_index);
    if (options->fresh) {
        device_path_remove(host->nvs_file);
        LOG_I("HOST", "fresh 模式：已删除 %s", host->nvs_file);
    }

    /* 5. 日志文件（--log-dir 指定时） */
    if (options->log_dir != NULL && options->log_dir[0] != '\0') {
        char log_dir[1024] = {0};
        if (copy_opt_string(options->log_dir, log_dir, sizeof(log_dir)) != DEMO_OK) {
            fail_rc = DEVICE_ERR_IO;
            err_set(error, fail_rc, 0, "create", "log_dir 路径规范化失败");
            goto fail;
        }
        if (device_path_mkdirs(log_dir) != DEMO_OK) {
            fail_rc = DEVICE_ERR_IO;
            err_set(error, fail_rc, 0, "create", "创建日志目录失败");
            goto fail;
        }
        char log_name[1024];
#ifdef _WIN32
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(log_name, sizeof(log_name),
                 "%s/device_dev%u_%04u%02u%02u_%02u%02u%02u.log", log_dir,
                 options->device_index, st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
#else
        {
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            char pattern[1024];
            snprintf(pattern, sizeof(pattern), "%s/device_dev%u_%%Y%%m%%d_%%H%%M%%S.log",
                     log_dir, options->device_index);
            strftime(log_name, sizeof(log_name), pattern, &tmv);
        }
#endif
        if (log_set_file(log_name) != DEMO_OK)
            LOG_W("HOST", "无法创建诊断日志文件：%s", log_name);
    }

    /* 6. 事件写入器（--events-jsonl 不可写 → 启动失败） */
    if (options->events_jsonl_path != NULL && options->events_jsonl_path[0] != '\0') {
        char events_path[1024] = {0};
        if (copy_opt_string(options->events_jsonl_path, events_path,
                            sizeof(events_path)) != DEMO_OK) {
            fail_rc = DEVICE_ERR_IO;
            err_set(error, fail_rc, 0, "create", "events_jsonl 路径规范化失败");
            goto fail;
        }
        host->events = device_events_open(events_path);
        if (host->events == NULL) {
            fail_rc = DEVICE_ERR_IO;
            err_set(error, fail_rc, 0, "create", "无法打开事件文件");
            goto fail;
        }
    }

    /* 6.1 sim catalog 目录（CLI 相对路径按启动 CWD 绝对化；sim 后端要求绝对路径） */
    if (options->sim_catalog_dir != NULL && options->sim_catalog_dir[0] != '\0') {
        if (copy_opt_string(options->sim_catalog_dir, host->sim_catalog_dir,
                            sizeof(host->sim_catalog_dir)) != DEMO_OK) {
            fail_rc = DEVICE_ERR_IO;
            err_set(error, fail_rc, 0, "create", "sim_catalog_dir 路径规范化失败");
            goto fail;
        }
    }

    /* 6.2 scenario 路径（CLI 相对路径按启动 CWD 绝对化） */
    if (options->scenario_path != NULL && options->scenario_path[0] != '\0') {
        if (copy_opt_string(options->scenario_path, host->scenario_path,
                            sizeof(host->scenario_path)) != DEMO_OK) {
            fail_rc = DEVICE_ERR_IO;
            err_set(error, fail_rc, 0, "create", "scenario 路径规范化失败");
            goto fail;
        }
    }

    /* 7. 队列与锁 */
    host->state_mutex = plat->mutex_create();
    host->snapshot_mutex = plat->mutex_create();
    host->cmds = device_cmd_queue_create(plat);
    host->logs = device_log_queue_create(plat);
    if (host->state_mutex == NULL || host->snapshot_mutex == NULL || host->cmds == NULL ||
        host->logs == NULL) {
        fail_rc = DEVICE_ERR_NO_MEMORY;
        err_set(error, fail_rc, 0, "create", "创建队列/锁失败");
        goto fail;
    }
    host->state = DEVICE_HOST_CREATED;

    /* 8. 快照初始字段（与设备无关部分） */
    host->snapshot.backend_kind = (int)options->backend;
    snprintf(host->snapshot.backend_name, sizeof(host->snapshot.backend_name), "%s",
             backend_name(options->backend));
    snprintf(host->device_id, sizeof(host->device_id), "02:00:00:00:00:%02X",
             options->device_index + 1);
    snprintf(host->snapshot.device_id, sizeof(host->snapshot.device_id), "%s",
             host->device_id);
    host->provision_port = host->cfg.use_real_wifi_sta
                               ? PROTO_TCP_PORT
                               : (int)(host->cfg.device_ap_port_base +
                                       options->device_index);
    host->snapshot.provision_port = host->provision_port;

    /* 9. 日志 sink 注册 */
    g_sink_host = host;
    log_set_sink(host_log_sink);

    /* 10. 后端工厂（宿主 dispatcher） */
    {
        device_sim_backend_options_t sim_opts;
        memset(&sim_opts, 0, sizeof(sim_opts));
        sim_opts.config_path = host->config_path[0] ? host->config_path : NULL;
        sim_opts.nvs_file = host->nvs_file;
        sim_opts.sim_catalog_dir = host->sim_catalog_dir[0] ? host->sim_catalog_dir
                                                            : NULL;
        sim_opts.target_ssid = host->cfg.target_ssid;
        sim_opts.target_password = host->cfg.target_password;
        sim_opts.host_virtual_ip = "127.0.0.1";
        sim_opts.device_index = options->device_index;
        sim_opts.provision_port = (unsigned int)host->provision_port;
        sim_opts.random_seed = (uint32_t)(options->device_index + 1) * 2654435761u;

        device_linux_backend_options_t linux_opts;
        memset(&linux_opts, 0, sizeof(linux_opts));
        linux_opts.config_path = host->config_path[0] ? host->config_path : NULL;
        linux_opts.nvs_file = host->nvs_file;
        linux_opts.hotspot_config_path = host->cfg.hs_config_path[0]
                                             ? host->cfg.hs_config_path
                                             : NULL;
        linux_opts.sta_interface = NULL;
        linux_opts.device_index = options->device_index;

        fail_rc = device_backend_create_for_host(options->backend, &sim_opts,
                                                 &linux_opts, &host->backend, error);
        if (fail_rc != DEVICE_OK) {
            if (error != NULL && error->operation[0] == '\0')
                snprintf(error->operation, sizeof(error->operation), "%s", "backend");
            host_set_last_error(host,
                                error != NULL ? error->message : "后端创建失败");
            goto fail;
        }
    }

    /* 11. net ctx（新契约：init 失败返回原错误并释放） */
    {
        int rc = net_ctx_create(host->backend.vtable, host->backend.user,
                                host->config_path[0] ? host->config_path : NULL,
                                &host->net);
        if (rc != DEMO_OK) {
            device_result_t mapped = map_demo_err(rc);
            fail_rc = (mapped == DEVICE_ERR_INTERNAL) ? DEVICE_ERR_BACKEND_INIT : mapped;
            err_set(error, fail_rc, 0, "create", "后端初始化失败");
            host_set_last_error(host, error != NULL ? error->message : "后端初始化失败");
            goto fail;
        }
    }

    /* 12. 设备核心 */
    host->app = device_app_create(&host->cfg, host->net, host->device_id,
                                  options->device_index);
    if (host->app == NULL) {
        fail_rc = DEVICE_ERR_NO_MEMORY;
        err_set(error, fail_rc, 0, "create", "设备核心创建失败");
        goto fail;
    }

    /* 13. 事件 sink 注入（core 埋点 → 事件文件 + scenario 引擎） */
    device_app_set_event_sink(host->app, host_core_event_sink, host);

    /* 14. scenario 引擎（--scenario；schema 非法 → 启动失败） */
    if (host->scenario_path[0] != '\0') {
        host->scenario = scenario_engine_create(host->scenario_path, host, error);
        if (host->scenario == NULL) {
            fail_rc = (error != NULL && error->code != DEVICE_ERR_NO_MEMORY)
                          ? error->code
                          : DEVICE_ERR_CONFIG_INVALID;
            if (error != NULL && error->operation[0] == '\0')
                snprintf(error->operation, sizeof(error->operation), "%s", "scenario");
            goto fail;
        }
    }

    host_snapshot_update(host);
    if (host->events != NULL || host->scenario != NULL) {
        char data[128];
        snprintf(data, sizeof(data), "{\"backend\":\"%s\",\"pid\":%ld}",
                 backend_name(options->backend), device_process_id());
        device_host_publish_event(host, "ready", "ok", 0, data);
    }
    *out_host = host;
    return DEVICE_OK;

fail:
    if (g_sink_host == host) {
        g_sink_host = NULL;
        log_set_sink(NULL);
    }
    if (host->events != NULL) {
        char data[512];
        snprintf(data, sizeof(data), "{\"operation\":\"%s\",\"message\":\"%s\"}",
                 error != NULL && error->operation[0] != '\0' ? error->operation : "create",
                 error != NULL ? error->message : "create_failed");
        device_events_write(host->events, "error", "fail", (int)fail_rc, data);
    }
    if (host->app != NULL)
        device_app_destroy(host->app);
    if (host->net != NULL)
        net_ctx_destroy(host->net);
    device_backend_instance_destroy(&host->backend);
    if (host->events != NULL)
        device_events_close(host->events);
    if (host->scenario != NULL)
        scenario_engine_destroy(host->scenario);
    if (host->cmds != NULL)
        device_cmd_queue_destroy(host->cmds);
    if (host->logs != NULL)
        device_log_queue_destroy(host->logs);
    if (host->state_mutex != NULL)
        plat->mutex_destroy(host->state_mutex);
    if (host->snapshot_mutex != NULL)
        plat->mutex_destroy(host->snapshot_mutex);
    free(host);
    *out_host = NULL;
    return fail_rc;
}

/* ---------------- start ---------------- */

device_result_t device_host_start(device_host_t *host, device_error_t *error)
{
    return device_host_start_internal(host, error, NULL);
}

device_result_t device_host_start_internal(device_host_t *host, device_error_t *error,
                                           const device_platform_t *thread_plat)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (host == NULL) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "start", "host 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    host->plat->mutex_lock(host->state_mutex);
    device_host_state_t st = host->state;
    host->plat->mutex_unlock(host->state_mutex);
    if (st != DEVICE_HOST_CREATED) {
        err_set(error, DEVICE_ERR_INVALID_STATE, 0, "start", "只有 CREATED 才能 start");
        return DEVICE_ERR_INVALID_STATE;
    }
    if (host->runner == NULL) {
        host->runner = device_runner_create(host->plat, host);
        if (host->runner == NULL) {
            err_set(error, DEVICE_ERR_NO_MEMORY, 0, "start", "创建 runner 失败");
            return DEVICE_ERR_NO_MEMORY;
        }
    }
    if (device_runner_start(host->runner, thread_plat) != DEMO_OK) {
        device_runner_destroy(host->runner);
        host->runner = NULL;
        err_set(error, DEVICE_ERR_THREAD, 0, "start", "创建工作线程失败");
        return DEVICE_ERR_THREAD; /* 保持 CREATED */
    }
    host->start_mono_ms = host->plat->monotonic_ms();
    host->plat->mutex_lock(host->state_mutex);
    host->state = DEVICE_HOST_RUNNING;
    host->plat->mutex_unlock(host->state_mutex);
    host_snapshot_update(host);
    return DEVICE_OK;
}

/* ---------------- request_stop / join / destroy ---------------- */

device_result_t device_host_request_stop(device_host_t *host)
{
    if (host == NULL)
        return DEVICE_ERR_INVALID_ARGUMENT;
    host->plat->mutex_lock(host->state_mutex);
    device_host_state_t st = host->state;
    host->plat->mutex_unlock(host->state_mutex);
    if (st == DEVICE_HOST_RUNNING) {
        if (host->runner != NULL)
            device_runner_request_stop(host->runner);
        host->plat->mutex_lock(host->state_mutex);
        host->state = DEVICE_HOST_STOP_REQUESTED;
        host->plat->mutex_unlock(host->state_mutex);
    }
    /* CREATED/STOP_REQUESTED/STOPPED：幂等 no-op */
    return DEVICE_OK;
}

device_result_t device_host_join(device_host_t *host, uint32_t timeout_ms,
                                 device_error_t *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (host == NULL) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "join", "host 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    host->plat->mutex_lock(host->state_mutex);
    device_host_state_t st = host->state;
    host->plat->mutex_unlock(host->state_mutex);
    if (st == DEVICE_HOST_CREATED || st == DEVICE_HOST_STOPPED)
        return DEVICE_OK; /* 无运行线程 */
    if (host->runner == NULL)
        return DEVICE_OK;
    int rc = device_runner_join(host->runner, timeout_ms);
    if (rc == DEVICE_PLAT_TIMEOUT) {
        err_set(error, DEVICE_ERR_TIMEOUT, 0, "join", "等待设备线程超时");
        return DEVICE_ERR_TIMEOUT; /* 句柄仍有效 */
    }
    return DEVICE_OK;
}

device_result_t device_host_destroy(device_host_t **host, device_error_t *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (host == NULL || *host == NULL)
        return DEVICE_OK; /* 幂等 */
    device_host_t *h = *host;

    h->plat->mutex_lock(h->state_mutex);
    device_host_state_t st = h->state;
    h->plat->mutex_unlock(h->state_mutex);
    if (st == DEVICE_HOST_RUNNING || st == DEVICE_HOST_STOP_REQUESTED) {
        err_set(error, DEVICE_ERR_BUSY, 0, "destroy", "设备仍在运行，请先 join");
        return DEVICE_ERR_BUSY;
    }

    /* 销毁顺序：core → net ctx → backend → 队列/锁/配置 */
    if (h->events != NULL || h->scenario != NULL) {
        char data[64];
        /* 进程退出码由 qt_main 决定，host 在销毁时只能提供停止是否干净 */
        snprintf(data, sizeof(data), "{\"exit_code\":%d}",
                 st == DEVICE_HOST_STOPPED ? 0 : (int)DEVICE_ERR_INTERNAL);
        device_host_publish_event(h, "shutdown_complete", "ok", 0, data);
    }
    if (g_sink_host == h) {
        g_sink_host = NULL;
        log_set_sink(NULL);
    }
    if (h->app != NULL) {
        device_app_destroy(h->app);
        h->app = NULL;
    }
    if (h->net != NULL) {
        net_ctx_destroy(h->net);
        h->net = NULL;
    }
    device_backend_instance_destroy(&h->backend);
    if (h->scenario != NULL) {
        scenario_engine_destroy(h->scenario);
        h->scenario = NULL;
    }
    if (h->events != NULL) {
        device_events_close(h->events);
        h->events = NULL;
    }
    if (h->runner != NULL) {
        device_runner_destroy(h->runner);
        h->runner = NULL;
    }
    if (h->cmds != NULL) {
        device_cmd_queue_destroy(h->cmds);
        h->cmds = NULL;
    }
    if (h->logs != NULL) {
        device_log_queue_destroy(h->logs);
        h->logs = NULL;
    }
    if (h->state_mutex != NULL) {
        h->plat->mutex_destroy(h->state_mutex);
        h->state_mutex = NULL;
    }
    if (h->snapshot_mutex != NULL) {
        h->plat->mutex_destroy(h->snapshot_mutex);
        h->snapshot_mutex = NULL;
    }
    free(h);
    *host = NULL;
    return DEVICE_OK;
}

/* ---------------- 查询 ---------------- */

device_result_t device_host_get_state(const device_host_t *host,
                                      device_host_state_t *out_state)
{
    if (host == NULL || out_state == NULL)
        return DEVICE_ERR_INVALID_ARGUMENT;
    host->plat->mutex_lock(host->state_mutex);
    *out_state = host->state;
    host->plat->mutex_unlock(host->state_mutex);
    return DEVICE_OK;
}

device_result_t device_host_get_snapshot(const device_host_t *host,
                                         device_snapshot_t *out_snapshot)
{
    if (host == NULL || out_snapshot == NULL)
        return DEVICE_ERR_INVALID_ARGUMENT;
    host->plat->mutex_lock(host->snapshot_mutex);
    *out_snapshot = host->snapshot;
    host->plat->mutex_unlock(host->snapshot_mutex);
    return DEVICE_OK;
}

/* ---------------- 命令 ---------------- */

device_result_t device_host_send_app_data(device_host_t *host, const char *utf8_text,
                                          size_t text_length, device_error_t *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (host == NULL) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "send_app_data", "host 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (utf8_text == NULL || text_length == 0 || text_length > APP_DATA_TEXT_MAX) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "send_app_data",
                "文本长度必须在 1~512 字节");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }

    host->plat->mutex_lock(host->state_mutex);
    device_host_state_t st = host->state;
    host->plat->mutex_unlock(host->state_mutex);
    if (st != DEVICE_HOST_RUNNING) {
        err_set(error, DEVICE_ERR_INVALID_STATE, 0, "send_app_data", "设备未在运行");
        host_set_last_error(host, "设备未在运行，无法发送 app_data");
        return DEVICE_ERR_INVALID_STATE;
    }
    host->plat->mutex_lock(host->snapshot_mutex);
    int session = host->snapshot.device_state == (int)DEV_STATE_SESSION;
    host->plat->mutex_unlock(host->snapshot_mutex);
    if (!session) {
        err_set(error, DEVICE_ERR_INVALID_STATE, 0, "send_app_data", "当前非会话状态");
        host_set_last_error(host, "当前非会话状态，无法发送 app_data");
        return DEVICE_ERR_INVALID_STATE;
    }

    device_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = DEVICE_CMD_APP_DATA;
    memcpy(cmd.app_data, utf8_text, text_length);
    cmd.app_data_len = text_length;
    if (device_cmd_queue_push(host->cmds, &cmd) == DEVICE_QUEUE_FULL) {
        err_set(error, DEVICE_ERR_BUSY, 0, "send_app_data", "命令队列已满");
        return DEVICE_ERR_BUSY;
    }
    return DEVICE_OK;
}

device_result_t device_host_inject_fault(device_host_t *host, const char *action,
                                         const char *argument_json,
                                         device_error_t *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (host == NULL) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "inject_fault", "host 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (action == NULL || action[0] == '\0') {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "inject_fault", "action 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    host->plat->mutex_lock(host->state_mutex);
    device_host_state_t st = host->state;
    host->plat->mutex_unlock(host->state_mutex);
    if (st != DEVICE_HOST_RUNNING) {
        err_set(error, DEVICE_ERR_INVALID_STATE, 0, "inject_fault", "设备未在运行");
        return DEVICE_ERR_INVALID_STATE;
    }
    device_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = DEVICE_CMD_INJECT_FAULT;
    snprintf(cmd.fault_action, sizeof(cmd.fault_action), "%s", action);
    if (argument_json != NULL)
        snprintf(cmd.fault_argument, sizeof(cmd.fault_argument), "%s", argument_json);
    if (device_cmd_queue_push(host->cmds, &cmd) == DEVICE_QUEUE_FULL) {
        err_set(error, DEVICE_ERR_BUSY, 0, "inject_fault", "命令队列已满");
        return DEVICE_ERR_BUSY;
    }
    return DEVICE_OK;
}

device_result_t device_host_drain_logs(device_host_t *host, device_log_record_t *records,
                                       size_t capacity, size_t *out_count,
                                       uint64_t *out_dropped)
{
    if (out_count != NULL)
        *out_count = 0;
    if (out_dropped != NULL)
        *out_dropped = 0;
    if (host == NULL || out_count == NULL || out_dropped == NULL)
        return DEVICE_ERR_INVALID_ARGUMENT;
    if (capacity > 0 && records == NULL)
        return DEVICE_ERR_INVALID_ARGUMENT;
    size_t count = 0;
    uint64_t dropped = 0;
    int rc = device_log_queue_pull(host->logs, records, capacity, &count, &dropped);
    *out_count = count;
    *out_dropped = dropped;
    return (rc == DEMO_OK) ? DEVICE_OK : DEVICE_ERR_INVALID_ARGUMENT;
}
