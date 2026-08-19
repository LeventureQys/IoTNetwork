#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "frame.h"
#include "protocol.h"

/* 从根级 demo/tests/test_frame.cpp / test_protocol.cpp 移植的契约测试 */

TEST(FrameWrap, Basic)
{
    const char *payload = "{}";
    uint8_t out[8];
    int n = frame_wrap((const uint8_t *)payload, 2, out, sizeof(out));
    ASSERT_EQ(n, 4);
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 2);
    EXPECT_EQ(out[2], '{');
    EXPECT_EQ(out[3], '}');
}

TEST(FrameWrap, TooLong)
{
    uint8_t payload[PROTO_MSG_MAX_LEN + 1];
    memset(payload, 'x', sizeof(payload));
    uint8_t out[PROTO_MSG_MAX_LEN + 8];
    EXPECT_EQ(frame_wrap(payload, (int)sizeof(payload), out, (int)sizeof(out)),
              DEMO_ERR_INVAL);
}

TEST(FrameWrap, CapTooSmall)
{
    uint8_t payload[16] = {0};
    uint8_t out[4];
    EXPECT_EQ(frame_wrap(payload, 16, out, sizeof(out)), DEMO_ERR_NOMEM);
}

TEST(FrameParse, SingleFrame)
{
    uint8_t out[8];
    int n = frame_wrap((const uint8_t *)"{}", 2, out, sizeof(out));
    ASSERT_EQ(n, 4);
    int off = 0, len = 0, consumed = 0;
    int r = frame_parse(out, n, &off, &len, &consumed);
    EXPECT_EQ(r, 1);
    EXPECT_EQ(off, 2);
    EXPECT_EQ(len, 2);
    EXPECT_EQ(consumed, 4);
}

TEST(FrameParse, TwoFramesSticky)
{
    uint8_t f1[16], f2[16];
    int n1 = frame_wrap((const uint8_t *)"{\"a\":1}", 7, f1, sizeof(f1));
    int n2 = frame_wrap((const uint8_t *)"{\"b\":2}", 7, f2, sizeof(f2));
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), f1, f1 + n1);
    buf.insert(buf.end(), f2, f2 + n2);

    int off = 0, len = 0, consumed = 0;
    int r = frame_parse(buf.data(), (int)buf.size(), &off, &len, &consumed);
    EXPECT_EQ(r, 1);
    EXPECT_EQ(len, 7);
    EXPECT_EQ(consumed, n1);
    EXPECT_EQ(memcmp(buf.data() + off, "{\"a\":1}", 7), 0);

    int remaining = (int)buf.size() - consumed;
    r = frame_parse(buf.data() + consumed, remaining, &off, &len, &consumed);
    EXPECT_EQ(r, 1);
    EXPECT_EQ(len, 7);
    EXPECT_EQ(consumed, remaining);
}

TEST(FrameParse, SplitPacket)
{
    uint8_t out[16];
    int n = frame_wrap((const uint8_t *)"{\"x\":1}", 7, out, sizeof(out));
    int off = 0, len = 0, consumed = 0;
    EXPECT_EQ(frame_parse(out, 1, &off, &len, &consumed), 0);
    EXPECT_EQ(frame_parse(out, 3, &off, &len, &consumed), 0);
    EXPECT_EQ(frame_parse(out, n, &off, &len, &consumed), 1);
    EXPECT_EQ(len, 7);
}

TEST(FrameParse, ZeroLengthMalformed)
{
    uint8_t data[2] = {0, 0};
    int off = 0, len = 0, consumed = 0;
    EXPECT_EQ(frame_parse(data, 2, &off, &len, &consumed), DEMO_ERR);
    EXPECT_EQ(consumed, 2);
}

TEST(FrameParse, TooLongMalformed)
{
    uint8_t data[4] = {0x04, 0x01, 'x', 'x'};
    int off = 0, len = 0, consumed = 0;
    EXPECT_EQ(frame_parse(data, 4, &off, &len, &consumed), DEMO_ERR);
    EXPECT_EQ(consumed, 2);
}

TEST(FrameParse, EmptyInput)
{
    int off = 0, len = 0, consumed = 0;
    EXPECT_EQ(frame_parse(nullptr, 0, &off, &len, &consumed), 0);
    EXPECT_EQ(frame_parse((const uint8_t *)"", 0, &off, &len, &consumed), 0);
}

TEST(FrameParse, NullArgs)
{
    uint8_t data[4] = {0, 2, '{', '}'};
    EXPECT_EQ(frame_parse(data, 4, nullptr, nullptr, nullptr), DEMO_ERR_INVAL);
}

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
