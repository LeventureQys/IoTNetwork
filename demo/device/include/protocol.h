#ifndef DEMO_PROTOCOL_H
#define DEMO_PROTOCOL_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* beta v1.1：一对一 PC 热点直连拓扑。本头文件是跨端契约的最终定义，
 * 与 demo/pc/include/pc/protocol.h 完全一致；禁止单端私自修改值。 */

#define PROTO_VERSION           1
#define PROTO_MSG_MAX_LEN       1024
#define PROTO_FRAME_HEAD_LEN    2
#define PROTO_TCP_PORT          5935
#define PROTO_PC_AP_IP          "192.168.137.1"
#define PROTO_PC_AP_PREFIX      "Modu_"
#define PROTO_PC_AP_DEFAULT_SSID "Modu_PC"
#define PROTO_PC_AP_PASSWORD    "modu_leventure"
#define PROTO_HOST_MAX_CONN     1
#define PROTO_SESSION_ID_LEN    8
#define APP_DATA_TEXT_MAX       512   /* 联调消息 UI 文本上限（字节，UTF-8；保证 app_data 帧不超 1024） */

/* beta v1.1：仅保留握手/心跳/应用数据 5 条命令 */
#define CMD_DEVICE_HELLO  "device_hello"
#define CMD_HOST_ACK      "host_ack"
#define CMD_PING          "ping"
#define CMD_PONG          "pong"
#define CMD_APP_DATA      "app_data"

typedef enum {
    WIFI_REASON_OK             = 0,
    WIFI_REASON_NO_AP_FOUND    = 201,
    WIFI_REASON_AUTH_FAIL      = 202,
    WIFI_REASON_HANDSHAKE_TIMEOUT = 205,
    WIFI_REASON_5G_BAND        = 500
} wifi_reason_t;

typedef enum {
    DEV_STATE_BOOT = 0,
    DEV_STATE_WIFI_SCAN,
    DEV_STATE_STA_JOIN,
    DEV_STATE_CONNECT,
    DEV_STATE_SESSION,
    DEV_STATE_HEAL,
    DEV_STATE_COUNT
} device_state_t;

const char *device_state_str(device_state_t s);

#ifdef __cplusplus
}
#endif

#endif
