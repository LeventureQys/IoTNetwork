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
    EXPECT_STREQ(device_state_str(DEV_STATE_WIFI_SCAN), "wifi_scan");
    EXPECT_STREQ(device_state_str(DEV_STATE_STA_JOIN), "sta_join");
    EXPECT_STREQ(device_state_str(DEV_STATE_CONNECT), "connect");
    EXPECT_STREQ(device_state_str(DEV_STATE_SESSION), "session");
    EXPECT_STREQ(device_state_str(DEV_STATE_HEAL), "heal");
    EXPECT_STREQ(device_state_str((device_state_t)999), "unknown");
}

TEST(Protocol, CommandNames)
{
    /* wire v2：握手/心跳/错误命令；app_data 保留历史兼容，生产数据面走 SERIAL_BYTES */
    EXPECT_STREQ(CMD_DEVICE_HELLO, "device_hello");
    EXPECT_STREQ(CMD_HOST_ACK, "host_ack");
    EXPECT_STREQ(CMD_PING, "ping");
    EXPECT_STREQ(CMD_PONG, "pong");
    EXPECT_STREQ(CMD_APP_DATA, "app_data");
    EXPECT_STREQ(CMD_ERROR, "error");
}

TEST(Protocol, WireV2Constants)
{
    EXPECT_EQ(PROTO_WIRE_VERSION, 2);
    EXPECT_EQ(PROTO_V2_HEAD_LEN, 4);
    EXPECT_EQ(PROTO_V2_BODY_HEAD_LEN, 12);
    EXPECT_EQ(PROTO_V2_FRAME_MAX_LEN, 65536);
    EXPECT_EQ(PROTO_V2_CONTROL_MAX_LEN, 4096);
    EXPECT_EQ(PROTO_V2_SERIAL_CHUNK_MAX, 16384);
    EXPECT_EQ(PROTO_V2_TYPE_CONTROL_JSON, 1);
    EXPECT_EQ(PROTO_V2_TYPE_SERIAL_BYTES, 2);
}

TEST(Protocol, ReasonCodes)
{
    EXPECT_EQ(WIFI_REASON_OK, 0);
    EXPECT_EQ(WIFI_REASON_NO_AP_FOUND, 201);
    EXPECT_EQ(WIFI_REASON_AUTH_FAIL, 202);
    EXPECT_EQ(WIFI_REASON_HANDSHAKE_TIMEOUT, 205);
    EXPECT_EQ(WIFI_REASON_5G_BAND, 500);
}

/* ---------------- wire v2 ---------------- */

TEST(FrameV2Wrap, ControlJson)
{
    const uint8_t payload[] = "{\"cmd\":\"ping\"}";
    uint8_t out[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 16];
    int n = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 1, payload, (int)sizeof(payload) - 1,
                          out, (int)sizeof(out));
    ASSERT_EQ(n, PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + (int)sizeof(payload) - 1);
    EXPECT_EQ(out[4], PROTO_V2_TYPE_CONTROL_JSON);
    EXPECT_EQ(out[5], 0);
    EXPECT_EQ(out[6], 0);
    EXPECT_EQ(out[7], 0);
    uint32_t total = ((uint32_t)out[0] << 24) | ((uint32_t)out[1] << 16) |
                     ((uint32_t)out[2] << 8) | out[3];
    EXPECT_EQ(total, (uint32_t)(PROTO_V2_BODY_HEAD_LEN + (int)sizeof(payload) - 1));
}

TEST(FrameV2Wrap, SerialBytesWithNul)
{
    uint8_t payload[5] = {0x00, 0xFF, 0x00, 0x01, 0x02};
    uint8_t out[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 8];
    int n = frame_v2_wrap(PROTO_V2_TYPE_SERIAL_BYTES, 7, payload, 5, out, (int)sizeof(out));
    ASSERT_EQ(n, PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 5);
    EXPECT_EQ(out[4], PROTO_V2_TYPE_SERIAL_BYTES);
    const uint8_t *body = out + PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN;
    EXPECT_EQ(memcmp(body, payload, 5), 0);
}

TEST(FrameV2Wrap, RejectsUnknownType)
{
    uint8_t out[64];
    EXPECT_EQ(frame_v2_wrap(99, 1, (const uint8_t *)"x", 1, out, (int)sizeof(out)),
              DEMO_ERR_INVAL);
}

TEST(FrameV2Wrap, RejectsEmptySerial)
{
    uint8_t out[64];
    EXPECT_EQ(frame_v2_wrap(PROTO_V2_TYPE_SERIAL_BYTES, 1, nullptr, 0, out, (int)sizeof(out)),
              DEMO_ERR_INVAL);
}

TEST(FrameV2Wrap, RejectsOversizedSerial)
{
    std::vector<uint8_t> payload(PROTO_V2_SERIAL_CHUNK_MAX + 1, 0);
    std::vector<uint8_t> out(PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN +
                             PROTO_V2_SERIAL_CHUNK_MAX + 1);
    EXPECT_EQ(frame_v2_wrap(PROTO_V2_TYPE_SERIAL_BYTES, 1, payload.data(),
                            (int)payload.size(), out.data(), (int)out.size()),
              DEMO_ERR_INVAL);
}

TEST(FrameV2Wrap, CapTooSmall)
{
    uint8_t out[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN - 1];
    EXPECT_EQ(frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 1, (const uint8_t *)"{}", 2,
                            out, (int)sizeof(out)),
              DEMO_ERR_NOMEM);
}

TEST(FrameV2Parse, Roundtrip)
{
    const uint8_t payload[] = {0x00, 0xFF, 0x7F};
    uint8_t out[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 8];
    int n = frame_v2_wrap(PROTO_V2_TYPE_SERIAL_BYTES, 0x0102030405060708ULL,
                          payload, 3, out, (int)sizeof(out));
    int type = 0, plen = 0, consumed = 0;
    uint64_t seq = 0;
    const uint8_t *pp = nullptr;
    int r = frame_v2_parse(out, n, &type, &seq, &pp, &plen, &consumed);
    EXPECT_EQ(r, 1);
    EXPECT_EQ(type, PROTO_V2_TYPE_SERIAL_BYTES);
    EXPECT_EQ(seq, 0x0102030405060708ULL);
    EXPECT_EQ(plen, 3);
    EXPECT_EQ(consumed, n);
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(memcmp(pp, payload, 3), 0);
}

TEST(FrameV2Parse, ByteByByte)
{
    const uint8_t payload[] = "hello";
    uint8_t frame[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 8];
    int n = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 2, payload, 5, frame, (int)sizeof(frame));
    int type = 0, plen = 0, consumed = 0;
    uint64_t seq = 0;
    const uint8_t *pp = nullptr;
    for (int i = 1; i < n; i++) {
        EXPECT_EQ(frame_v2_parse(frame, i, &type, &seq, &pp, &plen, &consumed), 0);
    }
    EXPECT_EQ(frame_v2_parse(frame, n, &type, &seq, &pp, &plen, &consumed), 1);
    EXPECT_EQ(type, PROTO_V2_TYPE_CONTROL_JSON);
    EXPECT_EQ(plen, 5);
}

TEST(FrameV2Parse, StickyFrames)
{
    uint8_t f1[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 8];
    uint8_t f2[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 8];
    int n1 = frame_v2_wrap(PROTO_V2_TYPE_SERIAL_BYTES, 1, (const uint8_t *)"abc", 3,
                           f1, (int)sizeof(f1));
    int n2 = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 2, (const uint8_t *)"{}", 2,
                           f2, (int)sizeof(f2));
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), f1, f1 + n1);
    buf.insert(buf.end(), f2, f2 + n2);

    int type = 0, plen = 0, consumed = 0;
    uint64_t seq = 0;
    const uint8_t *pp = nullptr;
    int r = frame_v2_parse(buf.data(), (int)buf.size(), &type, &seq, &pp, &plen, &consumed);
    EXPECT_EQ(r, 1);
    EXPECT_EQ(type, PROTO_V2_TYPE_SERIAL_BYTES);
    EXPECT_EQ(consumed, n1);
    int rest = (int)buf.size() - consumed;
    r = frame_v2_parse(buf.data() + consumed, rest, &type, &seq, &pp, &plen, &consumed);
    EXPECT_EQ(r, 1);
    EXPECT_EQ(type, PROTO_V2_TYPE_CONTROL_JSON);
    EXPECT_EQ(consumed, rest);
}

TEST(FrameV2Parse, LengthTooSmall)
{
    uint8_t data[PROTO_V2_HEAD_LEN] = {0, 0, 0, 5};
    int type = 0, plen = 0, consumed = 0;
    uint64_t seq = 0;
    const uint8_t *pp = nullptr;
    EXPECT_EQ(frame_v2_parse(data, (int)sizeof(data), &type, &seq, &pp, &plen, &consumed),
              DEMO_ERR);
}

TEST(FrameV2Parse, LengthTooLarge)
{
    uint8_t data[PROTO_V2_HEAD_LEN] = {0, 1, 0, 1};
    int type = 0, plen = 0, consumed = 0;
    uint64_t seq = 0;
    const uint8_t *pp = nullptr;
    EXPECT_EQ(frame_v2_parse(data, (int)sizeof(data), &type, &seq, &pp, &plen, &consumed),
              DEMO_ERR);
}

TEST(FrameV2Parse, UnknownType)
{
    uint8_t out[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 8];
    int n = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 1, (const uint8_t *)"{}", 2,
                          out, (int)sizeof(out));
    out[4] = 99;
    int type = 0, plen = 0, consumed = 0;
    uint64_t seq = 0;
    const uint8_t *pp = nullptr;
    EXPECT_EQ(frame_v2_parse(out, n, &type, &seq, &pp, &plen, &consumed), DEMO_ERR);
}

TEST(FrameV2Parse, NonZeroFlagsOrReserved)
{
    uint8_t out[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 8];
    int n = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 1, (const uint8_t *)"{}", 2,
                          out, (int)sizeof(out));
    out[5] = 1;
    int type = 0, plen = 0, consumed = 0;
    uint64_t seq = 0;
    const uint8_t *pp = nullptr;
    EXPECT_EQ(frame_v2_parse(out, n, &type, &seq, &pp, &plen, &consumed), DEMO_ERR);

    n = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 1, (const uint8_t *)"{}", 2,
                      out, (int)sizeof(out));
    out[7] = 1;
    EXPECT_EQ(frame_v2_parse(out, n, &type, &seq, &pp, &plen, &consumed), DEMO_ERR);
}

TEST(FrameV2Parse, NullArgs)
{
    uint8_t out[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 8];
    int n = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 1, (const uint8_t *)"{}", 2,
                          out, (int)sizeof(out));
    EXPECT_EQ(frame_v2_parse(out, n, nullptr, nullptr, nullptr, nullptr, nullptr),
              DEMO_ERR_INVAL);
}
