#ifndef DEMO_DEVICE_DEVICE_CONFIG_H
#define DEMO_DEVICE_DEVICE_CONFIG_H
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 设备业务配置（beta v1.1：一对一 PC 热点直连拓扑，裁剪旧配网/发现/多设备字段）。 */
typedef struct device_config {
    /* 上电与 WiFi 退避 */
    int power_on_jitter_max_ms;
    int wifi_retry_max;
    int wifi_backoff_base_ms;
    int wifi_backoff_cap_ms;
    int wifi_backoff_jitter_ms;
    /* 会话 */
    int heartbeat_interval_ms;
    int heartbeat_dead_ms;
    int busy_backoff_ms;
    int hello_timeout_ms;
    /* 重连退避 */
    int reconnect_backoff_base_ms;
    int reconnect_backoff_cap_ms;
    int reconnect_backoff_jitter_ms;
    /* 传输健壮性 */
    int malformed_max_per_conn;
    int device_rate_limit_per_sec;
    /* 跨端固定契约（值与 include/protocol.h 一致） */
    int   host_tcp_port;              /* 必须 5935 */
    char  pc_ap_ssid[33];             /* 默认 Modu_PC */
    char  pc_ap_password[64];         /* 默认 modu_leventure */
    char  pc_host_ip[16];             /* 默认 192.168.137.1 */
    /* 现存用途（事件日志等） */
    char  nvs_dir[512];
    /* 后端选择：0=sim 1=Linux 真实 WiFi STA */
    int   use_real_wifi_sta;
    /* 设备元信息 */
    char  device_fw_version[16];
    int   device_proto_ver;
    /* 运行 */
    int   duration_s;
    char  scenario_path[512];
    char  config_tag[32];
} device_config_t;

void device_config_defaults(device_config_t *cfg);

/* 兼容加载：json_path 缺失时使用内置默认并警告（out_missing=1）；
 * 未知/类型不匹配字段忽略；过长字符串按目标容量截断（历史语义）。
 * config_dir 输出配置文件所在绝对目录（文件缺失/未指定时为空串）。
 * 配置内相对路径（nvs_dir/scenario_path）按配置目录解析为绝对路径。
 * 返回 DEMO_OK / DEMO_ERR（JSON 非法或超大）/ DEMO_ERR_INVAL。 */
int device_config_load(device_config_t *cfg, const char *json_path,
                       char *config_dir, size_t config_dir_cap, int *out_missing);

/* 校验配置：NULL 参数或 error buffer 非法返回 DEMO_ERR_INVAL；
 * 固定值（SSID/密码/IP/端口）不符或退避参数非法返回 DEMO_ERR_INVAL，
 * error 为字段级中文原因，且绝不包含密码。 */
int device_config_validate(const device_config_t *cfg, char *error, int error_capacity);

#ifdef __cplusplus
}
#endif

#endif
