#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include "cJSON.h"
#include "protocol.h"

#ifndef PC_CONTRACT_GOLDEN
#define PC_CONTRACT_GOLDEN ""
#endif

static long json_int(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : -999999;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

TEST(Contract, GoldenSchema3)
{
    FILE *f = fopen(PC_CONTRACT_GOLDEN, "rb");
    ASSERT_NE(f, nullptr) << "contract golden not found: " << PC_CONTRACT_GOLDEN;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    ASSERT_GT(sz, 0);
    std::string buf((size_t)sz, '\0');
    ASSERT_EQ(fread(&buf[0], 1, (size_t)sz, f), (size_t)sz);
    fclose(f);

    cJSON *root = cJSON_Parse(buf.c_str());
    ASSERT_NE(root, nullptr);

    EXPECT_EQ(json_int(root, "schema_version"), 3);
    EXPECT_EQ(json_int(root, "protocol_version"), PROTO_VERSION);
    EXPECT_EQ(json_int(root, "wire_version"), PROTO_WIRE_VERSION);

    /* frame v2 */
    cJSON *frame = cJSON_GetObjectItemCaseSensitive(root, "frame");
    ASSERT_TRUE(cJSON_IsObject(frame));
    EXPECT_EQ(json_int(frame, "head_bytes"), PROTO_V2_HEAD_LEN);
    EXPECT_EQ(json_int(frame, "body_bytes"), PROTO_V2_BODY_HEAD_LEN);
    EXPECT_STREQ(json_str(frame, "byte_order"), "big");
    EXPECT_EQ(json_int(frame, "max_total_length"), PROTO_V2_FRAME_MAX_LEN);
    EXPECT_EQ(json_int(frame, "sequence_bytes"), 8);
    cJSON *types = cJSON_GetObjectItemCaseSensitive(frame, "types");
    ASSERT_TRUE(cJSON_IsObject(types));
    EXPECT_EQ(json_int(types, "control_json"), PROTO_V2_TYPE_CONTROL_JSON);
    EXPECT_EQ(json_int(types, "serial_bytes"), PROTO_V2_TYPE_SERIAL_BYTES);

    /* tcp */
    cJSON *tcp = cJSON_GetObjectItemCaseSensitive(root, "tcp");
    ASSERT_TRUE(cJSON_IsObject(tcp));
    EXPECT_EQ(json_int(tcp, "port"), PROTO_TCP_PORT);
    EXPECT_EQ(json_int(tcp, "max_connections"), PROTO_HOST_MAX_CONN);
    EXPECT_STREQ(json_str(tcp, "host_ip"), PROTO_PC_AP_IP);

    /* wifi */
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    ASSERT_TRUE(cJSON_IsObject(wifi));
    EXPECT_STREQ(json_str(wifi, "ssid_prefix"), PROTO_PC_AP_PREFIX);
    EXPECT_STREQ(json_str(wifi, "default_ssid"), PROTO_PC_AP_DEFAULT_SSID);
    EXPECT_STREQ(json_str(wifi, "password"), PROTO_PC_AP_PASSWORD);
    EXPECT_EQ(json_int(wifi, "prefix_length"), 24);

    /* limits */
    cJSON *limits = cJSON_GetObjectItemCaseSensitive(root, "limits");
    ASSERT_TRUE(cJSON_IsObject(limits));
    EXPECT_EQ(json_int(limits, "control_json_bytes"), PROTO_V2_CONTROL_MAX_LEN);
    EXPECT_EQ(json_int(limits, "serial_chunk_bytes"), PROTO_V2_SERIAL_CHUNK_MAX);
    EXPECT_EQ(json_int(limits, "app_data_text_bytes"), APP_DATA_TEXT_MAX);
    EXPECT_EQ(json_int(limits, "session_id_length"), PROTO_SESSION_ID_LEN);

    /* commands：握手/心跳/错误 5 条（app_data 移出生产数据面） */
    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(root, "commands");
    ASSERT_TRUE(cJSON_IsArray(cmds));
    ASSERT_EQ(cJSON_GetArraySize(cmds), 5);
    const char *expect_cmds[5] = {CMD_DEVICE_HELLO, CMD_HOST_ACK, CMD_PING, CMD_PONG,
                                  CMD_ERROR};
    for (int i = 0; i < 5; i++) {
        cJSON *item = cJSON_GetArrayItem(cmds, i);
        ASSERT_TRUE(cJSON_IsString(item));
        EXPECT_STREQ(item->valuestring, expect_cmds[i]);
    }

    /* serial_profile */
    cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "serial_profile");
    ASSERT_TRUE(cJSON_IsObject(profile));
    cJSON *fields = cJSON_GetObjectItemCaseSensitive(profile, "required_fields");
    ASSERT_TRUE(cJSON_IsArray(fields));
    ASSERT_EQ(cJSON_GetArraySize(fields), 5);
    cJSON *domains = cJSON_GetObjectItemCaseSensitive(profile, "value_domain");
    ASSERT_TRUE(cJSON_IsArray(domains));
    ASSERT_EQ(cJSON_GetArraySize(domains), 1);
    EXPECT_STREQ(cJSON_GetArrayItem(domains, 0)->valuestring, "raw_adc");

    /* device_states：六态数值正确 */
    cJSON *states = cJSON_GetObjectItemCaseSensitive(root, "device_states");
    ASSERT_TRUE(cJSON_IsObject(states));
    EXPECT_EQ(json_int(states, "boot"), DEV_STATE_BOOT);
    EXPECT_EQ(json_int(states, "wifi_scan"), DEV_STATE_WIFI_SCAN);
    EXPECT_EQ(json_int(states, "sta_join"), DEV_STATE_STA_JOIN);
    EXPECT_EQ(json_int(states, "connect"), DEV_STATE_CONNECT);
    EXPECT_EQ(json_int(states, "session"), DEV_STATE_SESSION);
    EXPECT_EQ(json_int(states, "heal"), DEV_STATE_HEAL);
    EXPECT_EQ(json_int(states, "count"), DEV_STATE_COUNT);

    cJSON_Delete(root);
}
