#include "device_heal.h"
#include "device_app.h"
#include "device_eventlog.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void heal_on_disconnect(device_app_t *app, const char *reason)
{
    evlog_record(app, "连接断开：%s", reason);
    /* 更新候选列表：当前 host 置顶 */
    if (app->sess_host.port != 0) {
        net_addr_t cur = app->sess_host;
        /* 已在首位则跳过 */
        if (app->candidate_count == 0 ||
            !(app->candidate_list[0].ip == cur.ip && app->candidate_list[0].port == cur.port)) {
            for (int i = app->candidate_count; i > 0; i--) {
                if (i >= PROTO_CANDIDATE_MAX)
                    continue;
                app->candidate_list[i] = app->candidate_list[i - 1];
            }
            app->candidate_list[0] = cur;
            if (app->candidate_count < PROTO_CANDIDATE_MAX)
                app->candidate_count++;
        }
        /* 持久化 */
        cJSON *root = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(root, "candidates");
        for (int i = 0; i < app->candidate_count; i++) {
            cJSON *it = cJSON_CreateObject();
            char ipbuf[32];
            snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                     app->candidate_list[i].ip & 0xFF,
                     (app->candidate_list[i].ip >> 8) & 0xFF,
                     (app->candidate_list[i].ip >> 16) & 0xFF,
                     (app->candidate_list[i].ip >> 24) & 0xFF);
            cJSON_AddStringToObject(it, "ip", ipbuf);
            cJSON_AddNumberToObject(it, "port", dev_ntohs(app->candidate_list[i].port));
            cJSON_AddNumberToObject(it, "last_seen", (int)(net_time_ms(app->net) / 1000));
            cJSON_AddItemToArray(arr, it);
        }
        char *s = cJSON_PrintUnformatted(root);
        if (s) {
            net_nvs_set(app->net, "host_candidates", (const uint8_t *)s, (int)strlen(s));
            free(s);
        }
        cJSON_Delete(root);
    }
    app->reconnect_attempt = 0;
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
    long delay = (long)base << n;
    if (delay > cap)
        delay = cap;
    delay += (long)(net_random(app->net) % (uint32_t)(app->params->reconnect_backoff_jitter_ms + 1));
    if (delay < 0)
        delay = cap;
    return (int)delay;
}

void heal_reset_backoff(device_app_t *app)
{
    app->reconnect_attempt = 0;
}

int heal_rssi_poll(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    if (now - app->last_rssi_sample_ms < (uint64_t)app->params->rssi_sample_interval_ms)
        return 0;
    app->last_rssi_sample_ms = now;

    int rssi = 0;
    if (net_wifi_get_rssi(app->net, &rssi) != DEMO_OK)
        return 0;
    if (rssi < app->params->rssi_bad_threshold_dbm) {
        if (app->rssi_bad_since_ms == 0)
            app->rssi_bad_since_ms = now;
        if (now - app->rssi_bad_since_ms >= (uint64_t)app->params->rssi_bad_duration_ms) {
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
