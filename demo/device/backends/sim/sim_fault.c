/* ============================================================================
 * sim_fault.c - 故障注入（纯 C11，显式实例，无全局状态）。
 *
 * 迁移自旧 net_sim/sim_backend.cpp 的 Inject()，action 名称与参数解析
 * 语义逐字保持（rssi_set 支持裸数字或 {"rssi":n}；sock_send_fail 支持
 * {"skip":n,"count":n}，非法参数回退默认值）。
 * ========================================================================== */
#include "sim_fault.h"
#include "sim_util.h"
#include "common.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sim_fault_init(sim_fault_t *f, sim_world_t *world, const char *tag,
                    char *sta_ssid, size_t sta_ssid_cap, int *sta_connected)
{
    memset(f, 0, sizeof(*f));
    f->world = world;
    f->tag = tag;
    f->sta_ssid = sta_ssid;
    f->sta_ssid_cap = sta_ssid_cap;
    f->sta_connected = sta_connected;
}

int sim_fault_should_fail_send(sim_fault_t *f)
{
    if (!f)
        return 0;
    if (f->send_fail_skip > 0) {
        f->send_fail_skip--;
        return 0;
    }
    if (f->send_fail_count > 0) {
        f->send_fail_count--;
        return 1;
    }
    return 0;
}

/* rssi_set：优先 JSON {"rssi":n}，其次裸十进制整数，否则默认 -80（旧语义） */
static int fault_parse_rssi(const char *arg_json)
{
    int rssi = -80;
    if (arg_json && arg_json[0]) {
        cJSON *j = cJSON_Parse(arg_json);
        if (j) {
            const cJSON *field = cJSON_GetObjectItemCaseSensitive(j, "rssi");
            if (cJSON_IsNumber(field))
                rssi = field->valueint;
            cJSON_Delete(j);
            return rssi;
        }
        int v = 0;
        if (sscanf(arg_json, "%d", &v) == 1)
            rssi = v;
    }
    return rssi;
}

int sim_fault_inject(sim_fault_t *f, const char *action, const char *arg_json)
{
    if (!f || !action)
        return DEMO_ERR_INVAL;
    sim_world_t *w = f->world;
    const char *tag = f->tag;

    if (strcmp(action, "wifi_disconnect") == 0) {
        sim_world_target_set_up(w, 0);
        return DEMO_OK;
    }
    if (strcmp(action, "wifi_ok") == 0) {
        sim_world_target_set_up(w, 1);
        return DEMO_OK;
    }
    if (strcmp(action, "wifi_auth_fail") == 0) {
        sim_world_target_set_auth_fail(w, 1);
        return DEMO_OK;
    }
    if (strcmp(action, "wifi_auth_ok") == 0) {
        sim_world_target_set_auth_fail(w, 0);
        return DEMO_OK;
    }
    if (strcmp(action, "wifi_ssid_mismatch") == 0) {
        if (!f->sta_connected || !*f->sta_connected)
            return DEMO_ERR;
        sim_util_copy_bounded(f->sta_ssid, f->sta_ssid_cap,
                              (arg_json && arg_json[0]) ? arg_json : "UnexpectedWifi");
        return DEMO_OK;
    }
    if (strcmp(action, "rssi_set") == 0) {
        sim_world_rssi_set(w, tag, fault_parse_rssi(arg_json));
        return DEMO_OK;
    }
    if (strcmp(action, "mcast_block") == 0) {
        sim_world_mcast_set_blocked(w, tag, 1);
        return DEMO_OK;
    }
    if (strcmp(action, "mcast_unblock") == 0) {
        sim_world_mcast_set_blocked(w, tag, 0);
        return DEMO_OK;
    }
    if (strcmp(action, "burst_send") == 0) {
        return DEMO_OK;
    }
    if (strcmp(action, "sock_send_fail") == 0) {
        int skip = 0, count = 1;
        cJSON *arg = (arg_json && arg_json[0]) ? cJSON_Parse(arg_json) : NULL;
        if (arg) {
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(arg, "skip");
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(arg, "count");
            if (cJSON_IsNumber(s))
                skip = s->valueint;
            if (cJSON_IsNumber(c))
                count = c->valueint;
            cJSON_Delete(arg);
        }
        if (skip < 0)
            skip = 0;
        if (count < 0)
            count = 0;
        f->send_fail_skip = skip;
        f->send_fail_count = count;
        return DEMO_OK;
    }
    return DEMO_ERR; /* 未知 action */
}
