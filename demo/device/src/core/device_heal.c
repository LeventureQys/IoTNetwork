#include "device_heal.h"
#include "device_app.h"
#include "device_eventlog.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RSSI 自愈阈值（beta v1.1 设备配置已裁剪 rssi_* 字段，采用固定产品常量）：
 * 连续 RSSI_BAD_DURATION_MS 低于 RSSI_BAD_THRESHOLD_DBM 触发主动重连。 */
#define RSSI_SAMPLE_INTERVAL_MS 5000
#define RSSI_BAD_THRESHOLD_DBM  -75
#define RSSI_BAD_DURATION_MS    30000

void heal_on_disconnect(device_app_t *app, const char *reason)
{
    evlog_record(app, "连接断开：%s", reason);
    app->reconnect_attempt = 0;
    app->heal_wait_ms = 0;
}

int heal_next_backoff_ms(device_app_t *app)
{
    if (app->busy_pending) {
        app->busy_pending = 0;
        return app->params->busy_backoff_ms;
    }
    int n = app->reconnect_attempt++;
    int base = app->params->reconnect_backoff_base_ms;
    int cap = app->params->reconnect_backoff_cap_ms;
    long delay = (long)base;
    for (int i = 0; i < n; i++) {
        delay *= 2;
        if (delay > (long)cap) {
            delay = (long)cap;
            break;
        }
    }
    if (delay > (long)cap)
        delay = (long)cap;
    delay += (long)(net_random(app->net) % (uint32_t)(app->params->reconnect_backoff_jitter_ms + 1));
    if (delay < 0)
        delay = cap;
    return (int)delay;
}

void heal_reset_backoff(device_app_t *app)
{
    app->reconnect_attempt = 0;
    app->heal_wait_ms = 0;
}

int heal_rssi_poll(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    if (now - app->last_rssi_sample_ms < (uint64_t)RSSI_SAMPLE_INTERVAL_MS)
        return 0;
    app->last_rssi_sample_ms = now;

    int rssi = 0;
    if (net_wifi_get_rssi(app->net, &rssi) != DEMO_OK)
        return 0;
    if (rssi < RSSI_BAD_THRESHOLD_DBM) {
        if (app->rssi_bad_since_ms == 0)
            app->rssi_bad_since_ms = now;
        if (now - app->rssi_bad_since_ms >= (uint64_t)RSSI_BAD_DURATION_MS) {
            LOG_W(app->device_id, "RSSI 持续偏低：%d dBm", rssi);
            evlog_record(app, "RSSI 持续偏低：%d", rssi);
            app->rssi_bad_since_ms = 0;
            app->rssi_bad_samples = 0;
            return 1;
        }
    } else {
        app->rssi_bad_since_ms = 0;
        app->rssi_bad_samples = 0;
    }
    return 0;
}
