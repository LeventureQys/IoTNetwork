#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "host_provision_client.h"
#include "pc_event.h"
#include "common.h"
#include "cJSON.h"

namespace {

/* 构造 wifi_result 帧；reason_json 为裸 JSON 值（如 "201"、"\u6bc6\u7801\u9519\u8bef"），
 * nullptr 表示帧内无 reason 字段。 */
cJSON *FrameWithReason(const char *reason_json)
{
    char buf[192];
    if (reason_json == nullptr)
        snprintf(buf, sizeof(buf), "{\"cmd\":\"wifi_result\",\"status\":\"fail\"}");
    else
        snprintf(buf, sizeof(buf),
                 "{\"cmd\":\"wifi_result\",\"status\":\"fail\",\"reason\":%s}",
                 reason_json);
    return cJSON_Parse(buf);
}

std::vector<std::string> ReadLines(const char *path)
{
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
            lines.push_back(line);
    return lines;
}

} // namespace

TEST(WifiResultReason, NumericReasonsPassThrough)
{
    /* 帧 reason 为数字时原样采用（201/202/205/500 等设备 reason code） */
    const int codes[] = {201, 202, 205, 500};
    for (int code : codes) {
        char reason[16];
        snprintf(reason, sizeof(reason), "%d", code);
        cJSON *frame = FrameWithReason(reason);
        ASSERT_NE(frame, nullptr);
        EXPECT_EQ(host_wifi_result_reason_from_frame(frame), code);
        cJSON_Delete(frame);
    }
}

TEST(WifiResultReason, StringReasonsMapped)
{
    /* 设备端帧 reason 为文案：映射为对应 wifi_reason_t */
    cJSON *no_ap = FrameWithReason("\"信号太弱\"");
    cJSON *auth = FrameWithReason("\"密码错误\"");
    cJSON *timeout = FrameWithReason("\"超时\"");
    cJSON *band5g = FrameWithReason("\"目标为5G网络\"");
    ASSERT_NE(no_ap, nullptr);
    ASSERT_NE(auth, nullptr);
    ASSERT_NE(timeout, nullptr);
    ASSERT_NE(band5g, nullptr);
    EXPECT_EQ(host_wifi_result_reason_from_frame(no_ap), WIFI_REASON_NO_AP_FOUND);
    EXPECT_EQ(host_wifi_result_reason_from_frame(auth), WIFI_REASON_AUTH_FAIL);
    EXPECT_EQ(host_wifi_result_reason_from_frame(timeout), WIFI_REASON_HANDSHAKE_TIMEOUT);
    EXPECT_EQ(host_wifi_result_reason_from_frame(band5g), WIFI_REASON_5G_BAND);
    cJSON_Delete(no_ap);
    cJSON_Delete(auth);
    cJSON_Delete(timeout);
    cJSON_Delete(band5g);
}

TEST(WifiResultReason, MissingOrUnknownFallbackMinusOne)
{
    /* 缺省/类型不符/未知文案 → 文档化缺省值 -1 */
    EXPECT_EQ(host_wifi_result_reason_from_frame(nullptr), -1);
    cJSON *no_reason = FrameWithReason(nullptr);
    cJSON *bad_type = FrameWithReason("{}");
    cJSON *unknown = FrameWithReason("\"设备内部错误\"");
    ASSERT_NE(no_reason, nullptr);
    ASSERT_NE(bad_type, nullptr);
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(host_wifi_result_reason_from_frame(no_reason), -1);
    EXPECT_EQ(host_wifi_result_reason_from_frame(bad_type), -1);
    EXPECT_EQ(host_wifi_result_reason_from_frame(unknown), -1);
    cJSON_Delete(no_reason);
    cJSON_Delete(bad_type);
    cJSON_Delete(unknown);
}

TEST(WifiResultReason, FailEventOutputMatchesFrame)
{
    /* 模拟 wifi_result 帧带 reason 201/202/205/500：事件输出 data.reason 与帧一致 */
    pc_events_close();
    const char *path = "run/test_wifi_result_reason.jsonl";
    std::filesystem::create_directories("run");
    std::remove(path);
    ASSERT_EQ(pc_events_open(path), DEMO_OK);

    const int codes[] = {201, 202, 205, 500};
    for (int code : codes) {
        char reason[16];
        snprintf(reason, sizeof(reason), "%d", code);
        cJSON *frame = FrameWithReason(reason);
        ASSERT_NE(frame, nullptr);
        host_emit_wifi_result_fail_event("02:00:00:00:00:01", frame);
        cJSON_Delete(frame);
    }
    pc_events_close();

    auto lines = ReadLines(path);
    ASSERT_EQ(lines.size(), (size_t)4);
    for (size_t i = 0; i < lines.size(); ++i) {
        cJSON *root = cJSON_Parse(lines[i].c_str());
        ASSERT_NE(root, nullptr) << lines[i];
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        EXPECT_TRUE(cJSON_IsString(event) &&
                    strcmp(event->valuestring, "wifi_result") == 0) << lines[i];
        EXPECT_TRUE(cJSON_IsString(result) &&
                    strcmp(result->valuestring, "fail") == 0) << lines[i];
        ASSERT_TRUE(cJSON_IsObject(data)) << lines[i];
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "status");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(data, "reason");
        EXPECT_TRUE(cJSON_IsString(status) && strcmp(status->valuestring, "fail") == 0)
            << lines[i];
        EXPECT_TRUE(cJSON_IsNumber(reason)) << lines[i];
        EXPECT_EQ(reason->valueint, codes[i]) << lines[i];
        cJSON_Delete(root);
    }
    std::remove(path);
    pc_events_close();
}
