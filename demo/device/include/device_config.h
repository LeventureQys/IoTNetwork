#ifndef DEMO_DEVICE_DEVICE_CONFIG_H
#define DEMO_DEVICE_DEVICE_CONFIG_H
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 设备业务配置（由共享 demo_params_t 拆分而来，字段名与历史值语义保持不变）。
 * 宿主 CLI/运行时、sim 后端、Linux 后端各自的配置见 device_host.h /
 * device_backend_factory.h。 */
typedef struct device_config {
    /* 阶段一 */
    int power_on_jitter_max_ms;
    int wifi_retry_max;
    int wifi_backoff_base_ms;
    int wifi_backoff_cap_ms;
    int wifi_backoff_jitter_ms;
    /* 阶段二 */
    int provision_auth_timeout_ms;
    int provision_wifi_cfg_timeout_ms;
    int provision_ap_idle_timeout_ms;
    int provision_ap_backoff_ms;
    int provision_pin_fail_max;
    int provision_confirm_window_ms;
    int provision_sta_try_max;
    int provision_handoff_grace_ms;
    /* 阶段三 */
    int discovery_fast_window_ms;
    int discovery_fast_interval_ms;
    int discovery_normal_interval_ms;
    int discovery_candidate_timeout_ms;
    /* 阶段四 */
    int heartbeat_interval_ms;
    int heartbeat_dead_ms;
    int host_max_conn;
    int busy_backoff_ms;
    int hello_timeout_ms;
    /* 阶段五 */
    int rssi_sample_interval_ms;
    int rssi_bad_threshold_dbm;
    int rssi_bad_duration_ms;
    int reconnect_backoff_base_ms;
    int reconnect_backoff_cap_ms;
    int reconnect_backoff_jitter_ms;
    int reconnect_to_discovery_ms;
    int watchdog_state_timeout_ms;
    /* 传输健壮性 */
    int malformed_max_per_conn;
    int device_rate_limit_per_sec;
    /* 网络 */
    int   host_tcp_port;
    char  host_virtual_ip[16];
    char  mcast_group[16];
    int   mcast_port;
    int   device_ap_port_base;
    char  nvs_dir[512];
    /* 真实 Linux 热点配置路径（net_linux/linux_hotspot.json） */
    char  hs_config_path[512];
    /* Linux 下使用真实 WiFi STA（nmcli），0=模拟 1=真实 */
    int   use_real_wifi_sta;
    /* 设备模拟 */
    int   device_count;
    char  target_ssid[33];
    char  target_password[64];
    int   target_band_2g;
    char  device_fw_version[16];
    int   device_proto_ver;
    int   duration_s;
    char  scenario_path[512];
    char  config_tag[32];
} device_config_t;

void device_config_defaults(device_config_t *cfg);

/* 兼容加载：json_path 缺失时使用内置默认并警告（out_missing=1）；
 * 未知/类型不匹配字段忽略；过长字符串按目标容量截断（历史语义）。
 * config_dir 输出配置文件所在绝对目录（文件缺失/未指定时为空串）。
 * 配置内相对路径（nvs_dir/hs_config_path/scenario_path）按配置目录解析为绝对路径。
 * 返回 DEMO_OK / DEMO_ERR（JSON 非法或超大）/ DEMO_ERR_INVAL。 */
int device_config_load(device_config_t *cfg, const char *json_path,
                       char *config_dir, size_t config_dir_cap, int *out_missing);

#ifdef __cplusplus
}
#endif

#endif
