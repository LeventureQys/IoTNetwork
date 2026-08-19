#include "params.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void params_defaults(demo_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->power_on_jitter_max_ms = 10000;
    p->wifi_retry_max = 5;
    p->wifi_backoff_base_ms = 1000;
    p->wifi_backoff_cap_ms = 30000;
    p->wifi_backoff_jitter_ms = 5000;
    p->provision_auth_timeout_ms = 10000;
    p->provision_wifi_cfg_timeout_ms = 60000;
    p->provision_ap_idle_timeout_ms = 120000;
    p->provision_ap_backoff_ms = 300000;
    p->provision_pin_fail_max = 5;
    p->provision_confirm_window_ms = 120000;
    p->provision_sta_try_max = 3;
    p->provision_handoff_grace_ms = 1000;
    p->discovery_fast_window_ms = 30000;
    p->discovery_fast_interval_ms = 500;
    p->discovery_normal_interval_ms = 1000;
    p->discovery_candidate_timeout_ms = 30000;
    p->heartbeat_interval_ms = 10000;
    p->heartbeat_dead_ms = 0;
    p->host_max_conn = 16;
    p->busy_backoff_ms = 60000;
    p->hello_timeout_ms = 5000;
    p->rssi_sample_interval_ms = 5000;
    p->rssi_bad_threshold_dbm = -75;
    p->rssi_bad_duration_ms = 30000;
    p->reconnect_backoff_base_ms = 1000;
    p->reconnect_backoff_cap_ms = 30000;
    p->reconnect_backoff_jitter_ms = 5000;
    p->reconnect_to_discovery_ms = 30000;
    p->watchdog_state_timeout_ms = 60000;
    p->malformed_max_per_conn = 3;
    p->device_rate_limit_per_sec = 50;
    p->host_tcp_port = 5935;
    snprintf(p->host_virtual_ip, sizeof(p->host_virtual_ip), "192.168.1.50");
    snprintf(p->mcast_group, sizeof(p->mcast_group), "%s", "224.0.2.1");
    p->mcast_port = 5936;
    p->device_ap_port_base = 20000;
    p->device_count = 1;
    snprintf(p->target_ssid, sizeof(p->target_ssid), "TactileFactory-2.4G");
    snprintf(p->target_password, sizeof(p->target_password), "securepass123");
    p->target_band_2g = 1;
    snprintf(p->device_fw_version, sizeof(p->device_fw_version), "1.0.0");
    p->device_proto_ver = 1;
    p->duration_s = 0;
    p->scenario_path[0] = '\0';
    p->config_tag[0] = '\0';
    p->runtime_dir[0] = '\0';
    p->sim_catalog_dir[0] = '\0';
    p->log_dir[0] = '\0';
}

static void copy_str(char *dst, int cap, const char *src)
{
    if (src == NULL)
        return;
    snprintf(dst, cap, "%s", src);
}

int params_load(demo_params_t *p, const char *json_path)
{
    if (p == NULL)
        return DEMO_ERR_INVAL;
    params_defaults(p);
    if (json_path == NULL || json_path[0] == '\0')
        return DEMO_OK;

    FILE *f = fopen(json_path, "rb");
    if (f == NULL)
        return DEMO_OK; /* 文件不存在：保持默认（缺失容错语义） */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return DEMO_ERR;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return DEMO_ERR_NOMEM;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL)
        return DEMO_ERR;

#define LOAD_INT(field) do { cJSON *v = cJSON_GetObjectItemCaseSensitive(root, #field); \
    if (cJSON_IsNumber(v)) p->field = v->valueint; } while (0)
#define LOAD_STR(field) do { cJSON *v = cJSON_GetObjectItemCaseSensitive(root, #field); \
    if (cJSON_IsString(v)) copy_str(p->field, (int)sizeof(p->field), v->valuestring); } while (0)

    LOAD_INT(power_on_jitter_max_ms);
    LOAD_INT(wifi_retry_max);
    LOAD_INT(wifi_backoff_base_ms);
    LOAD_INT(wifi_backoff_cap_ms);
    LOAD_INT(wifi_backoff_jitter_ms);
    LOAD_INT(provision_auth_timeout_ms);
    LOAD_INT(provision_wifi_cfg_timeout_ms);
    LOAD_INT(provision_ap_idle_timeout_ms);
    LOAD_INT(provision_ap_backoff_ms);
    LOAD_INT(provision_pin_fail_max);
    LOAD_INT(provision_confirm_window_ms);
    LOAD_INT(provision_sta_try_max);
    LOAD_INT(provision_handoff_grace_ms);
    LOAD_INT(discovery_fast_window_ms);
    LOAD_INT(discovery_fast_interval_ms);
    LOAD_INT(discovery_normal_interval_ms);
    LOAD_INT(discovery_candidate_timeout_ms);
    LOAD_INT(heartbeat_interval_ms);
    LOAD_INT(heartbeat_dead_ms);
    LOAD_INT(host_max_conn);
    LOAD_INT(busy_backoff_ms);
    LOAD_INT(hello_timeout_ms);
    LOAD_INT(rssi_sample_interval_ms);
    LOAD_INT(rssi_bad_threshold_dbm);
    LOAD_INT(rssi_bad_duration_ms);
    LOAD_INT(reconnect_backoff_base_ms);
    LOAD_INT(reconnect_backoff_cap_ms);
    LOAD_INT(reconnect_backoff_jitter_ms);
    LOAD_INT(reconnect_to_discovery_ms);
    LOAD_INT(watchdog_state_timeout_ms);
    LOAD_INT(malformed_max_per_conn);
    LOAD_INT(device_rate_limit_per_sec);
    LOAD_INT(host_tcp_port);
    LOAD_STR(host_virtual_ip);
    {
        cJSON *advertise = cJSON_GetObjectItemCaseSensitive(root, "host_advertise_ip");
        if (cJSON_IsString(advertise))
            copy_str(p->host_virtual_ip, (int)sizeof(p->host_virtual_ip), advertise->valuestring);
    }
    LOAD_STR(mcast_group);
    LOAD_INT(mcast_port);
    LOAD_INT(device_ap_port_base);
    LOAD_INT(device_count);
    LOAD_STR(target_ssid);
    LOAD_STR(target_password);
    LOAD_INT(target_band_2g);
    LOAD_STR(device_fw_version);
    LOAD_INT(device_proto_ver);
    LOAD_INT(duration_s);
    LOAD_STR(scenario_path);
    LOAD_STR(config_tag);
    LOAD_STR(runtime_dir);
    LOAD_STR(sim_catalog_dir);
    LOAD_STR(log_dir);

#undef LOAD_INT
#undef LOAD_STR

    cJSON_Delete(root);
    return DEMO_OK;
}
