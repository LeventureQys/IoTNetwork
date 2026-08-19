#include "device_runner.h"
#include "device_platform.h"
#include "device_host_internal.h"
#include "device_app.h"
#include "net_abstraction.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void runner_test_sleep_ms(int ms)
{
    if (ms > 0)
        Sleep((DWORD)ms);
}
#else
#include <time.h>
static void runner_test_sleep_ms(int ms)
{
    if (ms <= 0)
        return;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

/* 测试钩子：>0 时每出队一条命令处理前休眠（生产保持 0） */
int device_runner_test_cmd_hold_ms = 0;

struct device_runner {
    const device_platform_t *plat;
    device_host_t *host;
    device_thread_t *thread;
    device_mutex_t *stop_mutex;
    int stop_requested;
};

static void runner_process_commands(device_host_t *h);

static void runner_entry(void *user)
{
    device_runner_t *r = (device_runner_t *)user;
    device_host_t *h = r->host;

    for (;;) {
        if (device_runner_stop_requested(r))
            break;
        uint64_t now = h->plat->monotonic_ms();
        if (h->duration_ms > 0 && now - h->start_mono_ms >= h->duration_ms) {
            LOG_I(h->device_id, "运行时长达到配置上限，自动停止");
            device_runner_request_stop(r);
            break;
        }
        runner_process_commands(h);
        if (h->scenario != NULL)
            scenario_engine_tick(h->scenario, h);
        if (h->app == NULL || !device_app_run_step(h->app))
            break;
        device_host_snapshot_refresh(h);
    }

    if (h->app != NULL)
        device_app_request_stop(h->app);

    h->plat->mutex_lock(h->state_mutex);
    h->state = DEVICE_HOST_STOPPED;
    h->plat->mutex_unlock(h->state_mutex);
}

static void runner_process_commands(device_host_t *h)
{
    device_cmd_t cmd;
    while (device_cmd_queue_pop(h->cmds, &cmd) == DEVICE_QUEUE_OK) {
        if (device_runner_test_cmd_hold_ms > 0)
            runner_test_sleep_ms(device_runner_test_cmd_hold_ms);
        if (cmd.kind == DEVICE_CMD_APP_DATA) {
            if (cmd.app_data_len > 0 && cmd.app_data_len <= sizeof(h->pending_app_data)) {
                memcpy(h->pending_app_data, cmd.app_data, cmd.app_data_len);
                h->pending_app_data_len = cmd.app_data_len;
                h->pending_app_data_valid = 1;
            }
        } else if (cmd.kind == DEVICE_CMD_SERIAL_BYTES) {
            if (cmd.serial_data != NULL && cmd.serial_len > 0) {
                if (device_app_enqueue_serial_bytes(h->app, cmd.serial_data,
                                                    cmd.serial_len) != DEMO_OK) {
                    /* 非会话态/队列满：丢弃该 chunk（不跨 session 重放） */
                    LOG_D(h->device_id, "串口字节 chunk 丢弃（非会话态或队列满）");
                }
            }
            free(cmd.serial_data);
            cmd.serial_data = NULL;
        } else if (cmd.kind == DEVICE_CMD_INJECT_FAULT) {
            int rc = net_inject(h->net, cmd.fault_action, cmd.fault_argument);
            char data[160];
            snprintf(data, sizeof(data), "{\"action\":\"%s\",\"status\":\"%s\"}",
                     cmd.fault_action, rc == DEMO_OK ? "ok" : "fail");
            device_host_publish_event(h, "fault_applied", "ok", 0, data);
            if (rc != DEMO_OK)
                LOG_W(h->device_id, "故障注入失败：%s（rc=%d）", cmd.fault_action, rc);
        }
    }
    /* 待发 app_data 冲刷（限速失败保留待发，下轮重试） */
    if (h->pending_app_data_valid) {
        device_app_t *app = h->app;
        if (app == NULL || app->state != DEV_STATE_SESSION || app->sess_sock == NULL) {
            h->pending_app_data_valid = 0; /* 连接已不在，防悬挂 */
        } else {
            char buf[APP_DATA_TEXT_MAX + 1];
            memcpy(buf, h->pending_app_data, h->pending_app_data_len);
            buf[h->pending_app_data_len] = '\0';
            if (device_app_tx_text(app, buf) == DEMO_OK)
                h->pending_app_data_valid = 0;
        }
    }
}

device_runner_t *device_runner_create(const device_platform_t *plat, device_host_t *host)
{
    if (plat == NULL || host == NULL)
        return NULL;
    device_runner_t *r = (device_runner_t *)calloc(1, sizeof(device_runner_t));
    if (r == NULL)
        return NULL;
    r->plat = plat;
    r->host = host;
    r->stop_mutex = plat->mutex_create();
    if (r->stop_mutex == NULL) {
        free(r);
        return NULL;
    }
    return r;
}

void device_runner_destroy(device_runner_t *r)
{
    if (r == NULL)
        return;
    r->plat->mutex_destroy(r->stop_mutex);
    free(r);
}

int device_runner_start(device_runner_t *r, const device_platform_t *thread_plat)
{
    if (r == NULL)
        return DEMO_ERR_INVAL;
    const device_platform_t *tp = thread_plat != NULL ? thread_plat : r->plat;
    r->thread = tp->thread_create(runner_entry, r);
    if (r->thread == NULL)
        return DEMO_ERR;
    return DEMO_OK;
}

void device_runner_request_stop(device_runner_t *r)
{
    if (r == NULL)
        return;
    r->plat->mutex_lock(r->stop_mutex);
    r->stop_requested = 1;
    r->plat->mutex_unlock(r->stop_mutex);
}

int device_runner_stop_requested(device_runner_t *r)
{
    if (r == NULL)
        return 1;
    r->plat->mutex_lock(r->stop_mutex);
    int v = r->stop_requested;
    r->plat->mutex_unlock(r->stop_mutex);
    return v;
}

int device_runner_join(device_runner_t *r, uint32_t timeout_ms)
{
    if (r == NULL || r->thread == NULL)
        return DEVICE_PLAT_OK;
    int rc = r->plat->thread_join(r->thread, timeout_ms);
    return rc;
}
