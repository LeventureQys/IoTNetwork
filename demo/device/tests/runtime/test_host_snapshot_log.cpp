#include "test_host_fixture.h"

#include "device_log_queue.h"
#include "device_cmd_queue.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace {

struct SnapshotLogTest : ::testing::Test {
    std::string dir;
    void SetUp() override
    {
        fake_backend_reset();
        dir = TmpDir("snap");
        CleanDir(dir);
        WriteConfig(dir, "{\"power_on_jitter_max_ms\":50}");
    }
    void TearDown() override { CleanDir(dir); }
};

} // namespace

TEST_F(SnapshotLogTest, SnapshotConcurrentReadsConsistent)
{
    fake_backend_set_announce(1);
    PreseedCreds(dir, 0,
                 "{\"schema\":1,\"creds\":[{\"ssid\":\"TactileFactory-2.4G\","
                 "\"password\":\"securepass123\",\"confirmed\":1}]}");
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_TRUE(WaitDeviceState(h, DEV_STATE_SESSION, 10000));

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; t++) {
        readers.emplace_back([&]() {
            for (int i = 0; i < 200; i++) {
                device_snapshot_t s;
                if (device_host_get_snapshot(h, &s) != DEVICE_OK) {
                    bad.fetch_add(1);
                    continue;
                }
                /* 快照整体一致：host_state/device_state 均为合法枚举范围 */
                if (s.host_state != DEVICE_HOST_RUNNING &&
                    s.host_state != DEVICE_HOST_STOP_REQUESTED &&
                    s.host_state != DEVICE_HOST_STOPPED)
                    bad.fetch_add(1);
                if (s.device_state < 0 || s.device_state >= DEV_STATE_COUNT)
                    bad.fetch_add(1);
                if (strncmp(s.device_id, "02:00:00:00:00:01", 17) != 0)
                    bad.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }
    /* 读取期间执行一次生命周期操作 */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    for (auto &t : readers)
        t.join();
    EXPECT_EQ(bad.load(), 0);
    DestroyHost(&h);
}

TEST_F(SnapshotLogTest, SnapshotTracksProvisionState)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_TRUE(WaitDeviceState(h, DEV_STATE_AP_PROVISION, 10000));
    device_snapshot_t s;
    ASSERT_EQ(device_host_get_snapshot(h, &s), DEVICE_OK);
    EXPECT_EQ(s.device_state, DEV_STATE_AP_PROVISION);
    EXPECT_STREQ(s.ap_ssid, "Modu_0001");
    EXPECT_EQ(s.backend_kind, (int)DEVICE_BACKEND_SIM);
    EXPECT_STREQ(s.backend_name, "sim");
    EXPECT_EQ(s.session_online, 0);
    EXPECT_NE(s.provision_port, 0);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}

TEST_F(SnapshotLogTest, NoWritesAfterJoin)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_TRUE(WaitDeviceState(h, DEV_STATE_AP_PROVISION, 10000));
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);

    /* 快照冻结 */
    device_snapshot_t s1, s2;
    ASSERT_EQ(device_host_get_snapshot(h, &s1), DEVICE_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(device_host_get_snapshot(h, &s2), DEVICE_OK);
    EXPECT_EQ(memcmp(&s1, &s2, sizeof(s1)), 0);

    /* 日志不再增长 */
    size_t c1 = 0, c2 = 0;
    uint64_t d1 = 0, d2 = 0;
    device_log_record_t rec[64];
    ASSERT_EQ(device_host_drain_logs(h, rec, 64, &c1, &d1), DEVICE_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(device_host_drain_logs(h, rec, 64, &c2, &d2), DEVICE_OK);
    EXPECT_EQ(c2, 0u);
    EXPECT_EQ(d2, d1);
    DestroyHost(&h);
}

TEST_F(SnapshotLogTest, LogsArriveAndDrain)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_TRUE(WaitDeviceState(h, DEV_STATE_AP_PROVISION, 10000));
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    /* 运行期间产生了设备日志 */
    size_t count = 0;
    uint64_t dropped = 0;
    device_log_record_t rec[512];
    ASSERT_EQ(device_host_drain_logs(h, rec, 512, &count, &dropped), DEVICE_OK);
    EXPECT_GT(count, 0u);
    for (size_t i = 0; i < count; i++) {
        EXPECT_GT(rec[i].sequence, 0ull);
    }
    DestroyHost(&h);
}

TEST_F(SnapshotLogTest, DrainLogsArgValidation)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    size_t count = 0;
    uint64_t dropped = 0;
    device_log_record_t rec[4];
    EXPECT_EQ(device_host_drain_logs(nullptr, rec, 4, &count, &dropped),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_host_drain_logs(h, rec, 4, nullptr, &dropped),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_host_drain_logs(h, rec, 4, &count, nullptr),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_host_drain_logs(h, nullptr, 4, &count, &dropped),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_host_drain_logs(h, nullptr, 0, &count, &dropped), DEVICE_OK);
    DestroyHost(&h);
}

/* ---------------- 队列单元测试 ---------------- */

TEST_F(SnapshotLogTest, CmdQueueCapacity32NoOverwrite)
{
    device_cmd_queue_t *q = device_cmd_queue_create(&device_platform_default);
    ASSERT_NE(q, nullptr);
    device_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = DEVICE_CMD_APP_DATA;
    for (int i = 0; i < DEVICE_CMD_QUEUE_CAPACITY; i++) {
        snprintf(cmd.app_data, sizeof(cmd.app_data), "msg-%d", i);
        cmd.app_data_len = strlen(cmd.app_data);
        EXPECT_EQ(device_cmd_queue_push(q, &cmd), DEVICE_QUEUE_OK);
    }
    EXPECT_EQ(device_cmd_queue_count(q), 32);
    EXPECT_EQ(device_cmd_queue_push(q, &cmd), DEVICE_QUEUE_FULL); /* 满，不覆盖 */
    EXPECT_EQ(device_cmd_queue_count(q), 32);
    /* 弹出顺序保持 FIFO */
    for (int i = 0; i < DEVICE_CMD_QUEUE_CAPACITY; i++) {
        device_cmd_t out;
        memset(&out, 0, sizeof(out));
        EXPECT_EQ(device_cmd_queue_pop(q, &out), DEVICE_QUEUE_OK);
        char expected[64];
        snprintf(expected, sizeof(expected), "msg-%d", i);
        EXPECT_STREQ(out.app_data, expected);
    }
    EXPECT_EQ(device_cmd_queue_pop(q, &cmd), DEVICE_QUEUE_EMPTY);
    device_cmd_queue_destroy(q);
}

TEST_F(SnapshotLogTest, LogQueueOverflowDropped)
{
    device_log_queue_t *q = device_log_queue_create(&device_platform_default);
    ASSERT_NE(q, nullptr);
    for (int i = 0; i < 2000; i++)
        device_log_queue_push(q, LOG_INFO, "T", "spam");
    size_t count = 0;
    uint64_t dropped = 0;
    device_log_record_t rec[100];
    ASSERT_EQ(device_log_queue_pull(q, rec, 100, &count, &dropped), DEVICE_OK);
    EXPECT_EQ(count, 100u);
    /* 2000 push - 1024 容量 - 100 已拉 = 876 仍滞留；dropped ≥ 2000-1024 */
    EXPECT_GE(dropped, 976ull);
    /* 继续拉取直到空 */
    size_t total = 0;
    for (;;) {
        size_t c = 0;
        uint64_t d = 0;
        ASSERT_EQ(device_log_queue_pull(q, rec, 100, &c, &d), DEVICE_OK);
        total += c;
        if (c == 0)
            break;
    }
    EXPECT_EQ(total + 100u, 1024u);
    device_log_queue_destroy(q);
}

TEST_F(SnapshotLogTest, LogQueuePullSemantics)
{
    device_log_queue_t *q = device_log_queue_create(&device_platform_default);
    ASSERT_NE(q, nullptr);
    for (int i = 0; i < 5; i++)
        device_log_queue_push(q, LOG_INFO, "M", "rec");
    size_t count = 0;
    uint64_t dropped = 0;
    device_log_record_t rec[10];
    ASSERT_EQ(device_log_queue_pull(q, rec, 2, &count, &dropped), DEVICE_OK);
    EXPECT_EQ(count, 2u);
    EXPECT_EQ(dropped, 0ull);
    EXPECT_EQ(rec[0].sequence, 1ull);
    EXPECT_EQ(rec[1].sequence, 2ull);
    ASSERT_EQ(device_log_queue_pull(q, rec, 10, &count, &dropped), DEVICE_OK);
    EXPECT_EQ(count, 3u);
    EXPECT_EQ(rec[0].sequence, 3ull);
    ASSERT_EQ(device_log_queue_pull(q, rec, 10, &count, &dropped), DEVICE_OK);
    EXPECT_EQ(count, 0u);
    device_log_queue_destroy(q);
}

TEST_F(SnapshotLogTest, EventsJsonlWritten)
{
    std::string events_path = dir + "/events.jsonl";
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.events_jsonl_path = events_path.c_str();
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);

    FILE *f = fopen(events_path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    char line1[1024], line2[1024];
    ASSERT_NE(fgets(line1, sizeof(line1), f), nullptr);
    ASSERT_NE(fgets(line2, sizeof(line2), f), nullptr);
    fclose(f);
    EXPECT_NE(strstr(line1, "\"event\":\"ready\""), nullptr);
    EXPECT_NE(strstr(line2, "\"event\":\"shutdown_complete\""), nullptr);
    EXPECT_NE(strstr(line1, "\"role\":\"device\""), nullptr);
}
