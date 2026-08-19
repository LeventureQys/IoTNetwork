#ifndef DEMO_PROTOCOL_H
#define DEMO_PROTOCOL_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_VERSION         1
#define PROTO_MSG_MAX_LEN     1024
#define PROTO_FRAME_HEAD_LEN  2
#define PROTO_TCP_PORT        5935
#define PROTO_MCAST_GROUP     "224.0.2.1"
#define PROTO_MCAST_PORT      5936
#define PROTO_MDNS_TYPE       "_tactile._tcp"
#define PROTO_MDNS_INSTANCE   "host"
#define PROTO_HOST_MAX_CONN   16
#define PROTO_WIFI_CRED_MAX   2
#define PROTO_CANDIDATE_MAX   2
#define PROTO_SIM_AP_IP       "192.168.1.1"
#define PROTO_SESSION_ID_LEN  8
#define APP_DATA_TEXT_MAX     512   /* 联调消息 UI 文本上限（字节，UTF-8；保证 app_data 帧不超 1024） */

#define CMD_AUTH          "auth"
#define CMD_AUTH_RESULT   "auth_result"
#define CMD_WIFI_CONFIG   "wifi_config"
#define CMD_WIFI_RESULT   "wifi_result"
#define CMD_CLOSE_AP      "close_ap"
#define CMD_HOST_ANNOUNCE "host_announce"
#define CMD_HOST_BYE      "host_bye"
#define CMD_DEVICE_HELLO  "device_hello"
#define CMD_HOST_ACK      "host_ack"
#define CMD_PING          "ping"
#define CMD_PONG          "pong"
#define CMD_DIAG_QUERY    "diag_query"
#define CMD_DIAG_REPORT   "diag_report"
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
    DEV_STATE_STA_JOIN,
    DEV_STATE_AP_PROVISION,
    DEV_STATE_DISCOVERY,
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
