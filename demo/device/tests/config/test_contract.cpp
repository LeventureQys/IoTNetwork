#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include "cJSON.h"
#include "protocol.h"

#ifndef DEVICE_CONTRACT_GOLDEN
#define DEVICE_CONTRACT_GOLDEN ""
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

TEST(ProtocolConstants, Values)
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

TEST(ProtocolStates, EnumValues)
{
    EXPECT_EQ(DEV_STATE_BOOT, 0);
    EXPECT_EQ(DEV_STATE_WIFI_SCAN, 1);
    EXPECT_EQ(DEV_STATE_STA_JOIN, 2);
    EXPECT_EQ(DEV_STATE_CONNECT, 3);
    EXPECT_EQ(DEV_STATE_SESSION, 4);
    EXPECT_EQ(DEV_STATE_HEAL, 5);
    EXPECT_EQ(DEV_STATE_COUNT, 6);
}

TEST(ProtocolCommands, Names)
{
    EXPECT_STREQ(CMD_DEVICE_HELLO, "device_hello");
    EXPECT_STREQ(CMD_HOST_ACK, "host_ack");
    EXPECT_STREQ(CMD_PING, "ping");
    EXPECT_STREQ(CMD_PONG, "pong");
    EXPECT_STREQ(CMD_APP_DATA, "app_data");
}

TEST(Contract, GoldenSchema2)
{
    FILE *f = fopen(DEVICE_CONTRACT_GOLDEN, "rb");
    ASSERT_NE(f, nullptr) << "contract golden not found: " << DEVICE_CONTRACT_GOLDEN;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    ASSERT_GT(sz, 0);
    std::string buf((size_t)sz, '\0');
    ASSERT_EQ(fread(&buf[0], 1, (size_t)sz, f), (size_t)sz);
    fclose(f);

    cJSON *root = cJSON_Parse(buf.c_str());
    ASSERT_NE(root, nullptr);

    EXPECT_EQ(json_int(root, "schema_version"), 2);
    EXPECT_EQ(json_int(root, "protocol_version"), PROTO_VERSION);

    cJSON *tcp = cJSON_GetObjectItemCaseSensitive(root, "tcp");
    ASSERT_TRUE(cJSON_IsObject(tcp));
    EXPECT_EQ(json_int(tcp, "port"), PROTO_TCP_PORT);
    EXPECT_EQ(json_int(tcp, "max_connections"), PROTO_HOST_MAX_CONN);
    EXPECT_STREQ(json_str(tcp, "host_ip"), PROTO_PC_AP_IP);

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    ASSERT_TRUE(cJSON_IsObject(wifi));
    EXPECT_STREQ(json_str(wifi, "ssid_prefix"), PROTO_PC_AP_PREFIX);
    EXPECT_STREQ(json_str(wifi, "default_ssid"), PROTO_PC_AP_DEFAULT_SSID);
    EXPECT_STREQ(json_str(wifi, "password"), PROTO_PC_AP_PASSWORD);
    EXPECT_EQ(json_int(wifi, "prefix_length"), 24);

    cJSON *limits = cJSON_GetObjectItemCaseSensitive(root, "limits");
    ASSERT_TRUE(cJSON_IsObject(limits));
    EXPECT_EQ(json_int(limits, "app_data_text_bytes"), APP_DATA_TEXT_MAX);
    EXPECT_EQ(json_int(limits, "session_id_length"), PROTO_SESSION_ID_LEN);

    cJSON *cmds = cJSON_GetObjectItemCaseSensitive(root, "commands");
    ASSERT_TRUE(cJSON_IsArray(cmds));
    ASSERT_EQ(cJSON_GetArraySize(cmds), 5);
    const char *expect_cmds[5] = {CMD_DEVICE_HELLO, CMD_HOST_ACK, CMD_PING, CMD_PONG,
                                  CMD_APP_DATA};
    for (int i = 0; i < 5; i++) {
        cJSON *item = cJSON_GetArrayItem(cmds, i);
        ASSERT_TRUE(cJSON_IsString(item));
        EXPECT_STREQ(item->valuestring, expect_cmds[i]);
    }

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
