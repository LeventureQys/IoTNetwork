#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "pc_event.h"
#include "common.h"
#include "cJSON.h"

namespace {

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

cJSON *ParseLine(const std::string &line)
{
    return cJSON_Parse(line.c_str());
}

void EmitSample(const char *event, const char *data_json)
{
    pc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = event;
    ev.result = "ok";
    ev.code = 0;
    ev.device_id = "";
    ev.data_json = data_json;
    pc_events_emit(&ev);
}

} // namespace

TEST(Events, OpenFailureUnwritablePath)
{
    pc_events_close();
    const char *bad = "run/no_such_dir_9f3a/events.jsonl";
    std::filesystem::remove_all("run/no_such_dir_9f3a");
    EXPECT_NE(pc_events_open(bad), DEMO_OK);
    pc_events_close();
}

TEST(Events, DisabledWhenNotOpened)
{
    pc_events_close();
    EXPECT_EQ(pc_events_enabled(), 0);
    EmitSample("ready", nullptr); /* 不应崩溃 */
    pc_events_close();
}

TEST(Events, LinesHaveRequiredFieldsAndMonotonicSeq)
{
    pc_events_close();
    const char *path = "run/test_events_1.jsonl";
    std::remove(path);
    ASSERT_EQ(pc_events_open(path), DEMO_OK);
    EXPECT_EQ(pc_events_enabled(), 1);
    EmitSample("ready", "{\"backend\":\"sim\",\"pid\":1}");
    EmitSample("session_online", "{\"peer\":\"02:00:00:00:00:01\",\"reconnect_count\":0}");
    EmitSample("shutdown_complete", "{\"exit_code\":0}");
    pc_events_close();
    EXPECT_EQ(pc_events_enabled(), 0);

    auto lines = ReadLines(path);
    ASSERT_EQ(lines.size(), (size_t)3);
    int expected_seq = 1;
    const char *expected_events[] = {"ready", "session_online", "shutdown_complete"};
    for (size_t i = 0; i < lines.size(); ++i) {
        cJSON *root = ParseLine(lines[i]);
        ASSERT_NE(root, nullptr) << lines[i];
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        const cJSON *time_ms = cJSON_GetObjectItemCaseSensitive(root, "time_ms");
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        EXPECT_TRUE(cJSON_IsNumber(schema) && schema->valueint == 1) << lines[i];
        EXPECT_TRUE(cJSON_IsNumber(seq) && seq->valueint == expected_seq) << lines[i];
        EXPECT_TRUE(cJSON_IsNumber(time_ms)) << lines[i];
        EXPECT_TRUE(cJSON_IsString(role) && strcmp(role->valuestring, "pc") == 0) << lines[i];
        EXPECT_TRUE(cJSON_IsString(event) &&
                    strcmp(event->valuestring, expected_events[i]) == 0) << lines[i];
        EXPECT_TRUE(cJSON_IsString(result)) << lines[i];
        EXPECT_TRUE(cJSON_IsNumber(code)) << lines[i];
        EXPECT_TRUE(cJSON_IsObject(data)) << lines[i];
        expected_seq++;
        cJSON_Delete(root);
    }
    std::remove(path);
    pc_events_close();
}

TEST(Events, ShutdownCompleteLast)
{
    pc_events_close();
    const char *path = "run/test_events_2.jsonl";
    std::remove(path);
    ASSERT_EQ(pc_events_open(path), DEMO_OK);
    EmitSample("ready", nullptr);
    EmitSample("ping", "{\"direction\":\"rx\",\"sequence\":1}");
    EmitSample("shutdown_complete", "{\"exit_code\":0}");
    pc_events_close();
    auto lines = ReadLines(path);
    ASSERT_EQ(lines.size(), (size_t)3);
    cJSON *last = ParseLine(lines[2]);
    ASSERT_NE(last, nullptr);
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(last, "event");
    EXPECT_TRUE(cJSON_IsString(event) && strcmp(event->valuestring, "shutdown_complete") == 0);
    cJSON_Delete(last);
    std::remove(path);
    pc_events_close();
}

TEST(Events, ObserverAndWasSeen)
{
    pc_events_close();
    /* 用可观测副作用验证：observer 通过全局 vector 记录 */
    static std::vector<std::string> seen;
    seen.clear();
    pc_events_set_observer([](const char *event, uint64_t) { seen.push_back(event); });
    EmitSample("ready", nullptr);
    EmitSample("ping", "{\"direction\":\"rx\",\"sequence\":1}");
    pc_events_set_observer(nullptr);
    ASSERT_EQ(seen.size(), (size_t)2);
    EXPECT_EQ(seen[0], "ready");
    EXPECT_EQ(seen[1], "ping");
    EXPECT_TRUE(pc_events_was_seen("ready"));
    EXPECT_TRUE(pc_events_was_seen("ping"));
    EXPECT_FALSE(pc_events_was_seen("never_emitted"));
    pc_events_close();
}
