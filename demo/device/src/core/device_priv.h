#ifndef DEMO_DEVICE_PRIV_H
#define DEMO_DEVICE_PRIV_H

#include "net_abstraction.h"
#include "device_config.h"
#include "protocol.h"
#include "frame.h"
#include "device_atomic.h"
#include "cJSON.h"

/* 纯 C 字节序工具（零平台依赖，device 模块不引入任何平台头） */
static inline uint16_t dev_htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}
#define dev_ntohs dev_htons

typedef struct device_app device_app_t;

#define DEVICE_EVLOG_CAPACITY 50
#define DEVICE_EVLOG_TEXT_MAX 95

struct device_app {
    const device_config_t *params;
    net_ctx_t *net;
    char device_id[18];            /* "02:00:00:00:00:01" */
    int dev_index;                 /* 设备索引（0 起，AP 真实端口推导用） */
    char ap_ssid[33];              /* "Modu_XXXX" */
    char ap_password[64];          /* WPA2 密码（设备唯一） */
    char ap_pin[8];                /* 一次性 PIN（每次 AP 开启重新生成，认证成功即作废） */
    device_state_t state;

    /* 阶段一：凭据 */
    char creds[PROTO_WIFI_CRED_MAX][33];
    char cred_pass[PROTO_WIFI_CRED_MAX][64];
    int  cred_confirmed[PROTO_WIFI_CRED_MAX];
    int  cred_count;
    int  cred_active;
    int  wifi_retry_count;

    /* 阶段二：配网会话 */
    void *ap_listen;
    void *ap_conn;
    uint64_t ap_conn_start_ms;
    int  ap_pin_fail_count;
    int  provision_auth_ok;   /* PIN 认证通过（会话有效） */
    int  provision_wifi_ok;   /* wifi_config 成功（已连接目标 WiFi 并回复 ok） */
    uint64_t provision_confirm_deadline_ms; /* 未确认凭据必须在此之前连上上位机 */
    uint64_t provision_handoff_deadline_ms; /* wifi_result ok 后交接宽限期，到期主动停止 AP */

    /* 阶段三：发现 */
    uint64_t last_announce_seq;
    uint64_t discovery_fast_until_ms;
    int  discovery_round;          /* 0=mdns 1=mcast 2=candidate */
    net_addr_t sess_host;
    void *mcast_sock;
    uint64_t last_mcast_poll_ms;
    net_addr_t candidate_list[PROTO_CANDIDATE_MAX];
    int candidate_count;

    /* 阶段四：会话 */
    void *sess_sock;
    char  session_id[PROTO_SESSION_ID_LEN + 1];
    int   heartbeat_interval_ms;
    int   heartbeat_dead_ms;
    uint32_t ping_seq;
    uint64_t last_rx_ms;
    uint64_t last_ping_ms;
    int   session_ack_ok;
    int   malformed_count;
    uint64_t server_time_sync_ms;  /* 校时基准（本地单调时钟 - server_time*1000） */
    uint32_t app_data_seq;

    /* 阶段五：自愈 */
    int   reconnect_attempt;       /* -1 表示 busy 长退避挂起 */
    int   busy_pending;
    int   rssi_bad_samples;
    uint64_t rssi_bad_since_ms;
    uint64_t last_rssi_sample_ms;
    int   rate_burst_flag;

    /* 错误计数（diag_report） */
    int err_wifi_disconnects;
    int err_tcp_drops;
    int err_auth_fails;

    /* 通用（stop_flag 为原子标志：仅 device 线程读写、request_stop 跨线程置位） */
    dev_atomic_int_t stop_flag;
    uint64_t state_enter_ms;
    uint64_t uptime_start_ms;
    uint64_t next_tick_ms;
    uint64_t boot_backoff_until;   /* AP 退避到期时刻（回 BOOT 后等待） */
    uint64_t heal_enter_ms;        /* HEAL 状态起始（降级判定基准） */
    int   cred_confirm_done;       /* 本次会话凭据确认已写 */

    /* 状态快照（device 线程更新；runtime 同线程读取，经 facade 快照锁对外） */
    int snap_state;
    int snap_rssi;
    uint32_t snap_uptime_s;
    uint64_t last_snap_ms;

    /* 事件日志环形缓冲 */
    struct { char text[96]; uint32_t boot_s; } evlog[50];
    int evlog_head;
    int evlog_count;
    uint64_t last_evlog_nvs_ms;    /* NVS 写入防抖 */

    /* 帧接收缓冲（配网/业务连接共用） */
    uint8_t rx_buf[PROTO_MSG_MAX_LEN + PROTO_FRAME_HEAD_LEN + 1];
    int rx_len;
    void *rx_sock;

    /* 结构化事件上报 sink（runtime 注入；核心只负责埋点调用，不关心实现）。
     * ev_fn 可为 NULL（未指定 --events-jsonl / 无 scenario 时为空）。 */
    void (*ev_fn)(void *ev_user, const char *event, const char *result, int code,
                  const char *data_json);
    void *ev_user;
    uint32_t session_connect_count; /* 会话建立次数（session_online 的 reconnect_count） */
};

void device_creds_reload(device_app_t *app);

#endif
