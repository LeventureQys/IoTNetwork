#include "device_config.h"
#include "device_path.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void device_config_defaults(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->power_on_jitter_max_ms = 10000;
    cfg->wifi_retry_max = 5;
    cfg->wifi_backoff_base_ms = 1000;
    cfg->wifi_backoff_cap_ms = 30000;
    cfg->wifi_backoff_jitter_ms = 5000;
    cfg->provision_auth_timeout_ms = 10000;
    cfg->provision_wifi_cfg_timeout_ms = 60000;
    cfg->provision_ap_idle_timeout_ms = 120000;
    cfg->provision_ap_backoff_ms = 300000;
    cfg->provision_pin_fail_max = 5;
    cfg->provision_confirm_window_ms = 120000;
    cfg->provision_sta_try_max = 3;
    cfg->provision_handoff_grace_ms = 1000;
    cfg->discovery_fast_window_ms = 30000;
    cfg->discovery_fast_interval_ms = 500;
    cfg->discovery_normal_interval_ms = 1000;
    cfg->discovery_candidate_timeout_ms = 30000;
    cfg->heartbeat_interval_ms = 10000;
    cfg->heartbeat_dead_ms = 0;
    cfg->host_max_conn = 16;
    cfg->busy_backoff_ms = 60000;
    cfg->hello_timeout_ms = 5000;
    cfg->rssi_sample_interval_ms = 5000;
    cfg->rssi_bad_threshold_dbm = -75;
    cfg->rssi_bad_duration_ms = 30000;
    cfg->reconnect_backoff_base_ms = 1000;
    cfg->reconnect_backoff_cap_ms = 30000;
    cfg->reconnect_backoff_jitter_ms = 5000;
    cfg->reconnect_to_discovery_ms = 30000;
    cfg->watchdog_state_timeout_ms = 60000;
    cfg->malformed_max_per_conn = 3;
    cfg->device_rate_limit_per_sec = 50;
    cfg->host_tcp_port = 5935;
    snprintf(cfg->host_virtual_ip, sizeof(cfg->host_virtual_ip), "192.168.1.50");
    snprintf(cfg->mcast_group, sizeof(cfg->mcast_group), "%s", "224.0.2.1");
    cfg->mcast_port = 5936;
    cfg->device_ap_port_base = 20000;
    snprintf(cfg->nvs_dir, sizeof(cfg->nvs_dir), "run");
    snprintf(cfg->hs_config_path, sizeof(cfg->hs_config_path), "config/linux_hotspot.json");
    cfg->use_real_wifi_sta = 0;
    cfg->device_count = 1;
    snprintf(cfg->target_ssid, sizeof(cfg->target_ssid), "TactileFactory-2.4G");
    snprintf(cfg->target_password, sizeof(cfg->target_password), "securepass123");
    cfg->target_band_2g = 1;
    snprintf(cfg->device_fw_version, sizeof(cfg->device_fw_version), "1.0.0");
    cfg->device_proto_ver = 1;
    cfg->duration_s = 0;
    cfg->scenario_path[0] = '\0';
    cfg->config_tag[0] = '\0';
}

static void copy_str(char *dst, int cap, const char *src)
{
    if (src == NULL)
        return;
    snprintf(dst, cap, "%s", src);
}

/* 配置内相对路径：以配置文件目录解析为绝对路径（不依赖 CWD、不向上搜索）；
 * 绝对路径做平台风格规范化。 */
static void resolve_config_path(char *field, size_t field_cap, const char *config_dir)
{
    if (field[0] == '\0' || config_dir[0] == '\0')
        return;
    char resolved[1024];
    if (device_path_is_absolute(field)) {
        if (device_path_absolute(field, resolved, sizeof(resolved)) != DEMO_OK)
            return;
    } else {
        char joined[1024];
        if (device_path_join(config_dir, field, joined, sizeof(joined)) != DEMO_OK)
            return;
        if (device_path_absolute(joined, resolved, sizeof(resolved)) != DEMO_OK)
            return;
    }
    snprintf(field, field_cap, "%s", resolved);
}

int device_config_load(device_config_t *cfg, const char *json_path,
                       char *config_dir, size_t config_dir_cap, int *out_missing)
{
    if (cfg == NULL)
        return DEMO_ERR_INVAL;
    if (config_dir != NULL && config_dir_cap > 0)
        config_dir[0] = '\0';
    if (out_missing != NULL)
        *out_missing = 0;

    device_config_defaults(cfg);

    if (json_path == NULL || json_path[0] == '\0') {
        LOG_W("CONFIG", "未指定配置文件，使用内置默认参数");
        if (out_missing != NULL)
            *out_missing = 1;
        return DEMO_OK;
    }

    FILE *f = fopen(json_path, "rb");
    if (f == NULL) {
        LOG_W("CONFIG", "配置文件不存在（%s），使用内置默认参数", json_path);
        if (out_missing != NULL)
            *out_missing = 1;
        return DEMO_OK;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        LOG_W("CONFIG", "配置文件非法（大小异常：%ld）：%s", sz, json_path);
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
    if (root == NULL) {
        LOG_W("CONFIG", "配置文件 JSON 解析失败：%s", json_path);
        return DEMO_ERR;
    }

#define LOAD_INT(field) do { cJSON *v = cJSON_GetObjectItemCaseSensitive(root, #field); \
    if (cJSON_IsNumber(v)) cfg->field = v->valueint; } while (0)
#define LOAD_STR(field) do { cJSON *v = cJSON_GetObjectItemCaseSensitive(root, #field); \
    if (cJSON_IsString(v)) copy_str(cfg->field, (int)sizeof(cfg->field), v->valuestring); } while (0)

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
    LOAD_STR(mcast_group);
    LOAD_INT(mcast_port);
    LOAD_INT(device_ap_port_base);
    LOAD_STR(nvs_dir);
    LOAD_STR(hs_config_path);
    {
        cJSON *legacy = cJSON_GetObjectItemCaseSensitive(root, "linux_hotspot_config");
        if (cJSON_IsString(legacy))
            copy_str(cfg->hs_config_path, (int)sizeof(cfg->hs_config_path), legacy->valuestring);
    }
    LOAD_INT(use_real_wifi_sta);
    LOAD_INT(device_count);
    LOAD_STR(target_ssid);
    LOAD_STR(target_password);
    LOAD_INT(target_band_2g);
    LOAD_STR(device_fw_version);
    LOAD_INT(device_proto_ver);
    LOAD_INT(duration_s);
    LOAD_STR(scenario_path);
    LOAD_STR(config_tag);

#undef LOAD_INT
#undef LOAD_STR

    /* 配置文件目录（绝对） */
    if (config_dir != NULL && config_dir_cap > 0) {
        char dir[1024] = {0};
        if (device_path_dirname(json_path, dir, sizeof(dir)) == DEMO_OK &&
            dir[0] != '\0') {
            char abs[1024] = {0};
            if (device_path_absolute(dir, abs, sizeof(abs)) == DEMO_OK)
                snprintf(config_dir, config_dir_cap, "%s", abs);
        }
    }

    /* 配置内相对路径按配置目录解析 */
    resolve_config_path(cfg->nvs_dir, sizeof(cfg->nvs_dir), config_dir);
    resolve_config_path(cfg->hs_config_path, sizeof(cfg->hs_config_path), config_dir);
    resolve_config_path(cfg->scenario_path, sizeof(cfg->scenario_path), config_dir);

    cJSON_Delete(root);
    return DEMO_OK;
}
