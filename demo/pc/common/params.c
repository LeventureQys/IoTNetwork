#include "params.h"
#include "protocol.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void params_defaults(demo_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->heartbeat_interval_ms = 10000;
    p->heartbeat_dead_ms = 0;
    p->hello_timeout_ms = 5000;
    p->malformed_max_per_conn = 3;
    p->device_rate_limit_per_sec = 50;
    p->host_tcp_port = PROTO_TCP_PORT;
    snprintf(p->pc_ap_ssid, sizeof(p->pc_ap_ssid), "%s", PROTO_PC_AP_DEFAULT_SSID);
    snprintf(p->pc_ap_password, sizeof(p->pc_ap_password), "%s", PROTO_PC_AP_PASSWORD);
    snprintf(p->pc_ap_ip, sizeof(p->pc_ap_ip), "%s", PROTO_PC_AP_IP);
    p->pc_ap_prefix_length = 24;
    p->duration_s = 0;
    p->scenario_path[0] = '\0';
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

    LOAD_INT(heartbeat_interval_ms);
    LOAD_INT(heartbeat_dead_ms);
    LOAD_INT(hello_timeout_ms);
    LOAD_INT(malformed_max_per_conn);
    LOAD_INT(device_rate_limit_per_sec);
    LOAD_INT(host_tcp_port);
    LOAD_STR(pc_ap_ssid);
    LOAD_STR(pc_ap_password);
    LOAD_STR(pc_ap_ip);
    LOAD_INT(pc_ap_prefix_length);
    LOAD_INT(duration_s);
    LOAD_STR(scenario_path);
    LOAD_STR(runtime_dir);
    LOAD_STR(sim_catalog_dir);
    LOAD_STR(log_dir);

#undef LOAD_INT
#undef LOAD_STR

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

int params_validate(const demo_params_t *params, char *error, int error_capacity)
{
    if (params == NULL || error == NULL || error_capacity <= 0)
        return DEMO_ERR_INVAL;
    error[0] = '\0';

    if (params->heartbeat_interval_ms <= 0) {
        snprintf(error, error_capacity, "心跳间隔必须为正数");
        return DEMO_ERR_INVAL;
    }
    if (params->heartbeat_dead_ms < 0) {
        snprintf(error, error_capacity, "心跳失效阈值不能为负数");
        return DEMO_ERR_INVAL;
    }
    if (params->hello_timeout_ms <= 0) {
        snprintf(error, error_capacity, "握手超时必须为正数");
        return DEMO_ERR_INVAL;
    }
    if (params->pc_ap_prefix_length != 24) {
        snprintf(error, error_capacity, "PC热点前缀长度必须为 24");
        return DEMO_ERR_INVAL;
    }
    return validate_cross_fields(params->pc_ap_ssid, params->pc_ap_password,
                                 params->pc_ap_ip, params->host_tcp_port,
                                 error, error_capacity);
}
