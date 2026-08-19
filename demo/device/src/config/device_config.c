#include "device_config.h"
#include "protocol.h"
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
    cfg->heartbeat_interval_ms = 10000;
    cfg->heartbeat_dead_ms = 0;
    cfg->busy_backoff_ms = 60000;
    cfg->hello_timeout_ms = 5000;
    cfg->reconnect_backoff_base_ms = 1000;
    cfg->reconnect_backoff_cap_ms = 30000;
    cfg->reconnect_backoff_jitter_ms = 5000;
    cfg->malformed_max_per_conn = 3;
    cfg->device_rate_limit_per_sec = 50;
    cfg->host_tcp_port = PROTO_TCP_PORT;
    snprintf(cfg->pc_ap_ssid, sizeof(cfg->pc_ap_ssid), "%s", PROTO_PC_AP_DEFAULT_SSID);
    snprintf(cfg->pc_ap_password, sizeof(cfg->pc_ap_password), "%s", PROTO_PC_AP_PASSWORD);
    snprintf(cfg->pc_host_ip, sizeof(cfg->pc_host_ip), "%s", PROTO_PC_AP_IP);
    snprintf(cfg->nvs_dir, sizeof(cfg->nvs_dir), "run");
    cfg->use_real_wifi_sta = 0;
    snprintf(cfg->device_fw_version, sizeof(cfg->device_fw_version), "1.0.0");
    cfg->device_proto_ver = 1;
    cfg->serial_rows = 36;
    cfg->serial_cols = 44;
    cfg->serial_data_points = cfg->serial_rows * cfg->serial_cols;
    cfg->serial_frame_size = 4 + cfg->serial_data_points * 2;
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
    LOAD_INT(heartbeat_interval_ms);
    LOAD_INT(heartbeat_dead_ms);
    LOAD_INT(busy_backoff_ms);
    LOAD_INT(hello_timeout_ms);
    LOAD_INT(reconnect_backoff_base_ms);
    LOAD_INT(reconnect_backoff_cap_ms);
    LOAD_INT(reconnect_backoff_jitter_ms);
    LOAD_INT(malformed_max_per_conn);
    LOAD_INT(device_rate_limit_per_sec);
    LOAD_INT(host_tcp_port);
    LOAD_STR(pc_ap_ssid);
    LOAD_STR(pc_ap_password);
    LOAD_STR(pc_host_ip);
    LOAD_STR(nvs_dir);
    LOAD_INT(use_real_wifi_sta);
    LOAD_STR(device_fw_version);
    LOAD_INT(device_proto_ver);
    LOAD_INT(serial_frame_size);
    LOAD_INT(serial_rows);
    LOAD_INT(serial_cols);
    LOAD_INT(serial_data_points);
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
    resolve_config_path(cfg->scenario_path, sizeof(cfg->scenario_path), config_dir);

    cJSON_Delete(root);
    return DEMO_OK;
}

/* SSID/密码/IP/端口固定值校验（两端一致）；写字段级中文错误，绝不回显密码。 */
static int validate_cross_fields(const char *ssid, const char *password,
                                 const char *ip, int port,
                                 char *error, int error_capacity)
{
    size_t len = strlen(ssid);
    size_t prefix_len = strlen(PROTO_PC_AP_PREFIX);
    if (len < 6 || len > 32) {
        snprintf(error, error_capacity, "热点SSID长度必须为 6~32 字节");
        return DEMO_ERR_INVAL;
    }
    if (strncmp(ssid, PROTO_PC_AP_PREFIX, prefix_len) != 0) {
        snprintf(error, error_capacity, "热点SSID必须以 Modu_ 开头");
        return DEMO_ERR_INVAL;
    }
    for (size_t i = prefix_len; i < len; i++) {
        char c = ssid[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            snprintf(error, error_capacity, "热点SSID后缀仅允许字母/数字/短横线/下划线");
            return DEMO_ERR_INVAL;
        }
    }
    if (strcmp(password, PROTO_PC_AP_PASSWORD) != 0) {
        snprintf(error, error_capacity, "热点密码必须使用产品固定值");
        return DEMO_ERR_INVAL;
    }
    if (strcmp(ip, PROTO_PC_AP_IP) != 0) {
        snprintf(error, error_capacity, "PC热点IP必须为 192.168.137.1");
        return DEMO_ERR_INVAL;
    }
    if (port != PROTO_TCP_PORT) {
        snprintf(error, error_capacity, "TCP端口必须为 5935");
        return DEMO_ERR_INVAL;
    }
    return DEMO_OK;
}

int device_config_validate(const device_config_t *cfg, char *error, int error_capacity)
{
    if (cfg == NULL || error == NULL || error_capacity <= 0)
        return DEMO_ERR_INVAL;
    error[0] = '\0';

    if (cfg->wifi_retry_max < 1) {
        snprintf(error, error_capacity, "WiFi重试次数必须不小于 1");
        return DEMO_ERR_INVAL;
    }
    if (cfg->wifi_backoff_base_ms <= 0) {
        snprintf(error, error_capacity, "WiFi退避基数必须为正数");
        return DEMO_ERR_INVAL;
    }
    if (cfg->wifi_backoff_cap_ms < cfg->wifi_backoff_base_ms) {
        snprintf(error, error_capacity, "WiFi退避上限不能小于基数");
        return DEMO_ERR_INVAL;
    }
    if (cfg->wifi_backoff_jitter_ms < 0) {
        snprintf(error, error_capacity, "WiFi退避抖动不能为负数");
        return DEMO_ERR_INVAL;
    }
    if (cfg->reconnect_backoff_base_ms <= 0) {
        snprintf(error, error_capacity, "重连退避基数必须为正数");
        return DEMO_ERR_INVAL;
    }
    if (cfg->reconnect_backoff_cap_ms < cfg->reconnect_backoff_base_ms) {
        snprintf(error, error_capacity, "重连退避上限不能小于基数");
        return DEMO_ERR_INVAL;
    }
    if (cfg->reconnect_backoff_jitter_ms < 0) {
        snprintf(error, error_capacity, "重连退避抖动不能为负数");
        return DEMO_ERR_INVAL;
    }
    /* SSID/密码/IP/端口与 PC 完全一致（设备无前缀长度字段，固定 24 在 PC 侧校验） */
    return validate_cross_fields(cfg->pc_ap_ssid, cfg->pc_ap_password,
                                 cfg->pc_host_ip, cfg->host_tcp_port,
                                 error, error_capacity);
}
