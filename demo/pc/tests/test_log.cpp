#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include "log.h"

namespace {

/* 日志为进程级全局状态：用例串行执行，每例前后清理 sink 与文件。 */
void CleanLogState(const char *path)
{
    log_set_sink(nullptr);
    log_close_file();
    if (path && path[0] != '\0')
        std::remove(path);
}

int CountOccurrences(const char *path, const char *needle)
{
    std::ifstream in(path);
    std::string line;
    int count = 0;
    size_t needle_len = strlen(needle);
    while (std::getline(in, line)) {
        std::string::size_type pos = 0;
        while ((pos = line.find(needle, pos)) != std::string::npos) {
            count++;
            pos += needle_len;
        }
    }
    return count;
}

static int g_sink_calls = 0;
static void sink_probe(int, const char *, const char *msg)
{
    if (msg && strstr(msg, "LOG_MARKER_SINK_4C11"))
        g_sink_calls++;
}

} // namespace

TEST(LogFile, NullPathRejected)
{
    CleanLogState(nullptr);
    EXPECT_EQ(log_set_file(nullptr), DEMO_ERR_INVAL);
    EXPECT_EQ(log_set_file(""), DEMO_ERR_INVAL);
    CleanLogState(nullptr);
}

TEST(LogFile, MarkerWrittenExactlyOnce)
{
    const char *path = "run/test_log_single.log";
    std::filesystem::create_directories("run");
    CleanLogState(path);
    const char *marker = "LOG_MARKER_SINGLE_9F27";
    ASSERT_EQ(log_set_file(path), DEMO_OK);
    log_msg(LOG_INFO, "TEST", "%s", marker);
    log_close_file();
    EXPECT_EQ(CountOccurrences(path, marker), 1);
    std::remove(path);
    CleanLogState(nullptr);
}

TEST(LogFile, SinkDoesNotDuplicateFileLog)
{
    const char *path = "run/test_log_sink.log";
    std::filesystem::create_directories("run");
    CleanLogState(path);
    g_sink_calls = 0;
    log_set_sink(sink_probe);
    const char *marker = "LOG_MARKER_SINK_4C11";
    ASSERT_EQ(log_set_file(path), DEMO_OK);
    log_msg(LOG_INFO, "TEST", "%s", marker);
    log_close_file();
    EXPECT_EQ(g_sink_calls, 1);
    EXPECT_EQ(CountOccurrences(path, marker), 1);
    std::remove(path);
    CleanLogState(nullptr);
}
