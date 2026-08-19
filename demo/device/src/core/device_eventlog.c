#include "device_eventlog.h"
#include "cJSON.h"
#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>




static void evlog_nvs_persist(device_app_t *app, int force)
{
    uint64_t now = net_time_ms(app->net);
    if (!force && now - app->last_evlog_nvs_ms < 1000)
        return; /* 1s 防抖 */
    app->last_evlog_nvs_ms = now;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON *arr = cJSON_AddArrayToObject(root, "events");
    int idx = app->evlog_head;
    for (int i = 0; i < app->evlog_count; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "t", app->evlog[idx].boot_s);
        cJSON_AddStringToObject(it, "m", app->evlog[idx].text);
        cJSON_AddItemToArray(arr, it);
        idx = (idx + 1) % DEVICE_EVLOG_CAPACITY;
    }
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        net_nvs_set(app->net, "evlog", (const uint8_t *)s, (int)strlen(s));
        free(s);
    }
    cJSON_Delete(root);
}

void evlog_flush(device_app_t *app)
{
    if (app == NULL)
        return;
    evlog_nvs_persist(app, 1);
}

void evlog_record(device_app_t *app, const char *fmt, ...)
{
    if (app == NULL)
        return;
    char msg[DEVICE_EVLOG_TEXT_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    int idx = (app->evlog_head + app->evlog_count) % DEVICE_EVLOG_CAPACITY;
    snprintf(app->evlog[idx].text, sizeof(app->evlog[idx].text), "%s", msg);
    app->evlog[idx].boot_s = (uint32_t)((net_time_ms(app->net) - app->uptime_start_ms) / 1000);
    if (app->evlog_count < DEVICE_EVLOG_CAPACITY)
        app->evlog_count++;
    else
        app->evlog_head = (app->evlog_head + 1) % DEVICE_EVLOG_CAPACITY;

    LOG_I(app->device_id, "事件：%s", msg);
    evlog_nvs_persist(app, 0);
}

int evlog_fill_report(device_app_t *app, char *out, int cap)
{
    if (out == NULL || cap <= 0)
        return DEMO_ERR_INVAL;
    out[0] = '\0';
    int written = 0;
    int start = app->evlog_count > 10 ? app->evlog_count - 10 : 0;
    int idx = (app->evlog_head + start) % DEVICE_EVLOG_CAPACITY;
    for (int i = start; i < app->evlog_count; i++) {
        int need = snprintf(NULL, 0, "[%u]%s\n", app->evlog[idx].boot_s, app->evlog[idx].text);
        if (written + need + 1 > cap)
            break;
        written += snprintf(out + written, (size_t)(cap - written), "[%u]%s\n",
                            app->evlog[idx].boot_s, app->evlog[idx].text);
        idx = (idx + 1) % DEVICE_EVLOG_CAPACITY;
    }
    return DEMO_OK;
}
