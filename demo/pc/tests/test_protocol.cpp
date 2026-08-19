#include <gtest/gtest.h>
#include <cstring>
#include "protocol.h"

TEST(Protocol, Constants)
{
    EXPECT_EQ(PROTO_VERSION, 1);
    EXPECT_EQ(PROTO_MSG_MAX_LEN, 1024);
    EXPECT_EQ(PROTO_FRAME_HEAD_LEN, 2);
    EXPECT_EQ(PROTO_TCP_PORT, 5935);
    EXPECT_STREQ(PROTO_PC_AP_IP, "192.168.137.1");
    EXPECT_STREQ(PROTO_PC_AP_PREFIX, "Modu_");
    EXPECT_STREQ(PROTO_PC_AP_DEFAULT_SSID, "Modu_PC");
    EXPECT_STREQ(PROTO_PC_AP_PASSWORD, "modu_leventure");
    EXPECT_EQ(PROTO_HOST_MAX_CONN, 1);
    EXPECT_EQ(PROTO_SESSION_ID_LEN, 8);
    EXPECT_EQ(APP_DATA_TEXT_MAX, 512);
}

TEST(Protocol, StateStr)
{
    EXPECT_STREQ(device_state_str(DEV_STATE_BOOT), "boot");
    EXPECT_STREQ(device_state_str(DEV_STATE_WIFI_SCAN), "wifi_scan");
    EXPECT_STREQ(device_state_str(DEV_STATE_STA_JOIN), "sta_join");
    EXPECT_STREQ(device_state_str(DEV_STATE_CONNECT), "connect");
    EXPECT_STREQ(device_state_str(DEV_STATE_SESSION), "session");
    EXPECT_STREQ(device_state_str(DEV_STATE_HEAL), "heal");
    EXPECT_STREQ(device_state_str((device_state_t)999), "unknown");
}

TEST(Protocol, StateEnumValues)
{
    EXPECT_EQ(DEV_STATE_BOOT, 0);
    EXPECT_EQ(DEV_STATE_WIFI_SCAN, 1);
    EXPECT_EQ(DEV_STATE_STA_JOIN, 2);
    EXPECT_EQ(DEV_STATE_CONNECT, 3);
    EXPECT_EQ(DEV_STATE_SESSION, 4);
    EXPECT_EQ(DEV_STATE_HEAL, 5);
    EXPECT_EQ(DEV_STATE_COUNT, 6);
}

TEST(Protocol, CommandNames)
{
    /* beta v1.1：仅保留握手/心跳/应用数据 5 条命令 */
    EXPECT_STREQ(CMD_DEVICE_HELLO, "device_hello");
    EXPECT_STREQ(CMD_HOST_ACK, "host_ack");
    EXPECT_STREQ(CMD_PING, "ping");
    EXPECT_STREQ(CMD_PONG, "pong");
    EXPECT_STREQ(CMD_APP_DATA, "app_data");
}

TEST(Protocol, ReasonCodes)
{
    EXPECT_EQ(WIFI_REASON_OK, 0);
    EXPECT_EQ(WIFI_REASON_NO_AP_FOUND, 201);
    EXPECT_EQ(WIFI_REASON_AUTH_FAIL, 202);
    EXPECT_EQ(WIFI_REASON_HANDSHAKE_TIMEOUT, 205);
    EXPECT_EQ(WIFI_REASON_5G_BAND, 500);
}
