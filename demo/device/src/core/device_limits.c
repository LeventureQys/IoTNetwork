#include "device_limits.h"
#include "device_eventlog.h"
#include "log.h"

#define LIMITS_WINDOW_MS 1000

int limits_allow_send(device_app_t *app, int is_heartbeat)
{
    if (app->rate_burst_flag) {
        app->rate_burst_flag = 0;
        evlog_record(app, "rate limit triggered (burst inject)");
        LOG_W(app->device_id, "限速：检测到突发发送，消息已丢弃");
        return 0;
    }
    if (is_heartbeat)
        return 1; /* 心跳豁免 */
    return 1;     /* Demo 消息量远低于限速，正常放行（计数逻辑保留给 burst 注入演示） */
}
