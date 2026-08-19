#include "test_host_fixture.h"

#include <cstring>

namespace {

struct AppDataTest : ::testing::Test {
    std::string dir;
    void SetUp() override
    {
        fake_backend_reset();
        dir = TmpDir("appdata");
        CleanDir(dir);
        WriteConfig(dir, "{\"power_on_jitter_max_ms\":50}");
    }
    void TearDown() override { CleanDir(dir); }
};

/* 预置已确认凭据 + announce，等待进入 SESSION 状态 */
static device_host_t *StartSessionHost(const std::string &dir)
{
    fake_backend_set_announce(1);
    PreseedCreds(dir, 0,
                 "{\"schema\":1,\"creds\":[{\"ssid\":\"TactileFactory-2.4G\","
                 "\"password\":\"securepass123\",\"confirmed\":1}]}");
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    EXPECT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_TRUE(WaitDeviceState(h, DEV_STATE_SESSION, 10000));
    return h;
}

} // namespace

TEST_F(AppDataTest, LengthValidation)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    char big513[513];
    memset(big513, 'x', sizeof(big513));
    EXPECT_EQ(device_host_send_app_data(h, big513, 513, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_host_send_app_data(h, "hi", 0, &e), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_host_send_app_data(h, nullptr, 5, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    DestroyHost(&h);
}

TEST_F(AppDataTest, NotRunningInvalidState)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    EXPECT_EQ(device_host_send_app_data(h, "hi", 2, &e), DEVICE_ERR_INVALID_STATE);
    DestroyHost(&h);
}

TEST_F(AppDataTest, NotSessionInvalidState)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    /* 无凭据 → 停在配网/引导阶段，非会话态拒绝 */
    EXPECT_TRUE(WaitDeviceState(h, DEV_STATE_AP_PROVISION, 10000));
    EXPECT_EQ(device_host_send_app_data(h, "hi", 2, &e), DEVICE_ERR_INVALID_STATE);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}

TEST_F(AppDataTest, Send512Ok)
{
    device_host_t *h = StartSessionHost(dir);
    device_error_t e;
    char text512[513];
    memset(text512, 'a', 512);
    text512[512] = '\0';
    EXPECT_EQ(device_host_send_app_data(h, text512, 512, &e), DEVICE_OK);
    EXPECT_EQ(device_host_send_app_data(h, "hello", 5, &e), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}

TEST_F(AppDataTest, Send513Rejected)
{
    device_host_t *h = StartSessionHost(dir);
    device_error_t e;
    char text513[513];
    memset(text513, 'a', 513);
    EXPECT_EQ(device_host_send_app_data(h, text513, 513, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}

TEST_F(AppDataTest, AfterStopInvalidState)
{
    device_host_t *h = StartSessionHost(dir);
    device_error_t e;
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_STOPPED);
    EXPECT_EQ(device_host_send_app_data(h, "hi", 2, &e), DEVICE_ERR_INVALID_STATE);
    DestroyHost(&h);
}

TEST_F(AppDataTest, QueueFullBusy)
{
    device_host_t *h = StartSessionHost(dir);
    /* 确定性消费者节流：runner 每出队一条命令处理前休眠 1000ms。
     * 40 条突发提交（亚毫秒级）期间消费者至多弹出 1 条 → 队列必达满态。 */
    device_runner_test_cmd_hold_ms = 1000;
    device_error_t e;
    std::string text(512, 'q');
    int ok = 0;
    device_result_t rc = DEVICE_OK;
    bool saw_busy = false;
    for (int i = 0; i < 40; i++) {
        rc = device_host_send_app_data(h, text.c_str(), text.size(), &e);
        if (rc == DEVICE_OK)
            ok++;
        else if (rc == DEVICE_ERR_BUSY)
            saw_busy = true;
    }
    /* 容量 32：成功数不超过 32+突发期间排空量（≤1）；必现 BUSY 且不覆盖 */
    EXPECT_TRUE(saw_busy);
    EXPECT_GE(ok, 32);
    EXPECT_LE(ok, 33);
    EXPECT_EQ(rc, DEVICE_ERR_BUSY);
    device_runner_test_cmd_hold_ms = 0;
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 10000, &e), DEVICE_OK);
    DestroyHost(&h);
}

TEST_F(AppDataTest, InjectFaultQueuedAndExecuted)
{
    device_host_t *h = StartSessionHost(dir);
    device_error_t e;
    EXPECT_EQ(device_host_inject_fault(h, "burst", "{}", &e), DEVICE_OK);
    EXPECT_EQ(device_host_inject_fault(h, "drop", nullptr, &e), DEVICE_OK);
    /* runner 执行注入；轮询直到生效 */
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (strcmp(fake_backend_last_inject(), "drop") == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_STREQ(fake_backend_last_inject(), "drop");
    EXPECT_EQ(device_host_inject_fault(h, "", "{}", &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}
