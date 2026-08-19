#ifndef DEMO_DEVICE_DEVICE_HOST_H
#define DEMO_DEVICE_DEVICE_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct device_host device_host_t;

typedef enum device_result {
    DEVICE_OK = 0,
    DEVICE_ERR_INVALID_ARGUMENT = -1,
    DEVICE_ERR_INVALID_STATE = -2,
    DEVICE_ERR_NO_MEMORY = -3,
    DEVICE_ERR_CONFIG_NOT_FOUND = -4,
    DEVICE_ERR_CONFIG_INVALID = -5,
    DEVICE_ERR_IO = -6,
    DEVICE_ERR_BACKEND_UNAVAILABLE = -7,
    DEVICE_ERR_BACKEND_INIT = -8,
    DEVICE_ERR_THREAD = -9,
    DEVICE_ERR_TIMEOUT = -10,
    DEVICE_ERR_BUSY = -11,
    DEVICE_ERR_NOT_SUPPORTED = -12,
    DEVICE_ERR_INTERNAL = -13
} device_result_t;

typedef enum device_host_state {
    DEVICE_HOST_CREATED = 0,
    DEVICE_HOST_RUNNING,
    DEVICE_HOST_STOP_REQUESTED,
    DEVICE_HOST_STOPPED
} device_host_state_t;

typedef enum device_backend_kind {
    DEVICE_BACKEND_SIM = 0,
    DEVICE_BACKEND_LINUX,
    DEVICE_BACKEND_ESP32C2
} device_backend_kind_t;

typedef struct device_error {
    device_result_t code;
    int platform_code;
    char operation[64];
    char message[256];
} device_error_t;

typedef struct device_host_options {
    const char *config_path;
    device_backend_kind_t backend;
    unsigned int device_index;
    int fresh;
    unsigned int duration_seconds;
    const char *runtime_dir;
    const char *sim_catalog_dir;
    const char *log_dir;
    const char *events_jsonl_path;
    const char *scenario_path;
} device_host_options_t;

/* join 超时特殊值：无限等待 */
#define DEVICE_JOIN_WAIT_FOREVER UINT32_MAX

typedef struct device_snapshot {
    device_host_state_t host_state;
    int device_state;
    int rssi_dbm;
    uint32_t uptime_seconds;
    int backend_kind;
    int session_online;
    char device_id[18];
    char target_ssid[33];     /* 配置的目标 PC 热点 SSID */
    char pc_host_ip[16];      /* 配置的固定上位机 IPv4 */
    int pc_host_port;         /* 固定 TCP 端口（5935） */
    char backend_name[32];
    char last_error[256];
} device_snapshot_t;

typedef struct device_log_record {
    uint64_t sequence;
    uint64_t timestamp_ms;
    int level;
    char module[32];
    char message[512];
} device_log_record_t;

void device_host_options_init(device_host_options_t *options);
device_result_t device_host_options_parse_argv(device_host_options_t *options, int argc, char *const argv[], device_error_t *error);
device_result_t device_host_create(const device_host_options_t *options, device_host_t **out_host, device_error_t *error);
device_result_t device_host_start(device_host_t *host, device_error_t *error);
device_result_t device_host_request_stop(device_host_t *host);
device_result_t device_host_join(device_host_t *host, uint32_t timeout_ms, device_error_t *error);
device_result_t device_host_destroy(device_host_t **host, device_error_t *error);
device_result_t device_host_get_state(const device_host_t *host, device_host_state_t *out_state);
device_result_t device_host_get_snapshot(const device_host_t *host, device_snapshot_t *out_snapshot);
device_result_t device_host_send_app_data(device_host_t *host, const char *utf8_text, size_t text_length, device_error_t *error);

/* wire v2：提交一段不透明二进制串口字节流（1..PROTO_V2_SERIAL_CHUNK_MAX）。
 * 调用时复制入有界队列；成功仅表示 accepted，不表示已写 socket。
 * 队列满返回 DEVICE_ERR_BUSY；非会话态返回 DEVICE_ERR_INVALID_STATE。 */
device_result_t device_host_send_serial_bytes(device_host_t *host, const uint8_t *bytes,
                                              size_t length, device_error_t *error);
device_result_t device_host_inject_fault(device_host_t *host, const char *action, const char *argument_json, device_error_t *error);
device_result_t device_host_drain_logs(device_host_t *host, device_log_record_t *records, size_t capacity, size_t *out_count, uint64_t *out_dropped);

#ifdef __cplusplus
}
#endif

#endif
