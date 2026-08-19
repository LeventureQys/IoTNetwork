#ifndef PC_PARAMS_H
#define PC_PARAMS_H
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* beta v1.1：一对一 PC 热点直连拓扑配置（裁剪旧配网/发现/多设备字段）。 */
typedef struct demo_params {
    /* 会话 */
    int heartbeat_interval_ms;
    int heartbeat_dead_ms;
    int hello_timeout_ms;
    /* 传输健壮性 */
    int malformed_max_per_conn;
    int device_rate_limit_per_sec;
    /* 跨端固定契约（值与 device/include/protocol.h 一致） */
    int   host_tcp_port;              /* 必须 5935 */
    char  pc_ap_ssid[33];             /* 默认 Modu_PC */
    char  pc_ap_password[64];         /* 默认 modu_leventure */
    char  pc_ap_ip[16];               /* 默认 192.168.137.1 */
    int   pc_ap_prefix_length;        /* 默认 24 */
    /* 运行 */
    int   duration_s;
    char  scenario_path[260];
    char  runtime_dir[260];
    char  sim_catalog_dir[260];
    char  log_dir[260];
} demo_params_t;

void params_defaults(demo_params_t *p);
int  params_load(demo_params_t *p, const char *json_path);

/* 校验配置：NULL 参数或 error buffer 非法返回 DEMO_ERR_INVAL；
 * 固定值（SSID/密码/IP/前缀长度/端口）不符返回 DEMO_ERR_INVAL，
 * error 为字段级中文原因，且绝不包含密码。 */
int  params_validate(const demo_params_t *params, char *error, int error_capacity);

#ifdef __cplusplus
}
#endif

#endif
