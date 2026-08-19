#include <gtest/gtest.h>
#include <cstring>
#include "protocol.h"

TEST(Protocol, StateStr)
{
    EXPECT_STREQ(device_state_str(DEV_STATE_BOOT), "boot");
    EXPECT_STREQ(device_state_str(DEV_STATE_STA_JOIN), "sta_join");
    EXPECT_STREQ(device_state_str(DEV_STATE_AP_PROVISION), "ap_provision");
    EXPECT_STREQ(device_state_str(DEV_STATE_DISCOVERY), "discovery");
    EXPECT_STREQ(device_state_str(DEV_STATE_CONNECT), "connect");
    EXPECT_STREQ(device_state_str(DEV_STATE_SESSION), "session");
    EXPECT_STREQ(device_state_str(DEV_STATE_HEAL), "heal");
    EXPECT_STREQ(device_state_str((device_state_t)999), "unknown");
}

TEST(Protocol, CommandNames)
{
    /* 13 种标准命令与协议文档 2.4 命令空间一致 */
    EXPECT_STREQ(CMD_AUTH, "auth");
    EXPECT_STREQ(CMD_AUTH_RESULT, "auth_result");
    EXPECT_STREQ(CMD_WIFI_CONFIG, "wifi_config");
    EXPECT_STREQ(CMD_WIFI_RESULT, "wifi_result");
    EXPECT_STREQ(CMD_CLOSE_AP, "close_ap");
    EXPECT_STREQ(CMD_HOST_ANNOUNCE, "host_announce");
    EXPECT_STREQ(CMD_HOST_BYE, "host_bye");
    EXPECT_STREQ(CMD_DEVICE_HELLO, "device_hello");
    EXPECT_STREQ(CMD_HOST_ACK, "host_ack");
    EXPECT_STREQ(CMD_PING, "ping");
    EXPECT_STREQ(CMD_PONG, "pong");
    EXPECT_STREQ(CMD_DIAG_QUERY, "diag_query");
    EXPECT_STREQ(CMD_DIAG_REPORT, "diag_report");
    /* 通用业务占位 */
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
