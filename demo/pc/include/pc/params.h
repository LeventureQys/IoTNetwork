#ifndef PC_PARAMS_H
#define PC_PARAMS_H
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct demo_params {
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
    /* 设备模拟（PC 侧保留字段，兼容既有参数语义） */
    int   device_count;
    char  target_ssid[33];
    char  target_password[64];
    int   target_band_2g;
    char  device_fw_version[16];
    int   device_proto_ver;
    int   duration_s;
    char  scenario_path[260];
    char  config_tag[32];
    /* PC 自包含路径（beta v1.0.5）：空串表示未配置，由入口按运行/配置目录补默认 */
    char  runtime_dir[260];
    char  sim_catalog_dir[260];
    char  log_dir[260];
} demo_params_t;

void params_defaults(demo_params_t *p);
int  params_load(demo_params_t *p, const char *json_path);

#ifdef __cplusplus
}
#endif

#endif
