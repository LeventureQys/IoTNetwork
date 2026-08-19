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
#define DEVICE_SCAN_MAX_APS 32

/* wire v2 发送队列（runner 单线程，无锁）。 */
#define DEV_TX_QUEUE_CAP 64

struct dev_tx_item {
    uint8_t *data;   /* malloc 编码后的完整帧（所有权归队列） */
    int len;
};

struct device_app {
    const device_config_t *params;
    net_ctx_t *net;
    char device_id[18];            /* "02:00:00:00:00:01" */
    int dev_index;                 /* 设备索引（0 起） */
    device_state_t state;

    /* 固定上位机地址（启动期由配置 pc_host_ip:host_tcp_port 写入，网络字节序）。
     * v1.1 一对一拓扑：禁止从 gateway/discovery 推导。 */
    net_addr_t sess_host;

    /* 会话（单一业务连接） */
    void *sess_sock;
    char  session_id[PROTO_SESSION_ID_LEN + 1];
    int   heartbeat_interval_ms;
    int   heartbeat_dead_ms;
    uint32_t ping_seq;
    uint64_t last_rx_ms;
    uint64_t last_ping_ms;
    int   session_ack_ok;
    int   malformed_count;
    uint32_t app_data_seq;

    /* 自愈/计数 */
    int   reconnect_attempt;       /* TCP 重连退避计数 */
    int   busy_pending;            /* host_ack busy 长退避挂起（一次性消费） */
    int   heal_wait_ms;            /* HEAL 当前退避等待时长（进入 HEAL 时置 0） */
    int   wifi_retry_count;        /* WiFi 扫描/连接失败计数（驱动扫描退避） */
    int   rssi_bad_samples;
    uint64_t rssi_bad_since_ms;
    uint64_t last_rssi_sample_ms;
    int   rate_burst_flag;

    /* 错误计数 */
    int err_wifi_disconnects;
    int err_tcp_drops;
    int err_auth_fails;

    /* 通用（stop_flag 为原子标志：仅 device 线程读写、request_stop 跨线程置位） */
    dev_atomic_int_t stop_flag;
    uint64_t state_enter_ms;
    uint64_t uptime_start_ms;
    uint64_t next_tick_ms;
    uint64_t heal_enter_ms;        /* HEAL 状态起始（观测用） */

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

    /* 帧接收缓冲（业务连接） */
    uint8_t rx_buf[PROTO_MSG_MAX_LEN + PROTO_FRAME_HEAD_LEN + 1];
    int rx_len;
    void *rx_sock;

    /* wire v2 发送队列与 in-flight（partial write 续传） */
    struct dev_tx_item tx_queue[DEV_TX_QUEUE_CAP];
    int tx_head;
    int tx_count;
    uint8_t *tx_inflight_data;
    int tx_inflight_len;
    int tx_inflight_off;
    uint64_t tx_sequence;

    /* 结构化事件上报 sink（runtime 注入；核心只负责埋点调用，不关心实现）。
     * ev_fn 可为 NULL（未指定 --events-jsonl / 无 scenario 时为空）。 */
    void (*ev_fn)(void *ev_user, const char *event, const char *result, int code,
                  const char *data_json);
    void *ev_user;
    uint32_t session_connect_count; /* 会话建立次数（session_online 的 reconnect_count） */
};

#endif
