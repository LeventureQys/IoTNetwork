#include "test_host_fixture.h"

#include <cstring>

namespace {

struct FailureTest : ::testing::Test {
    std::string dir;
    void SetUp() override
    {
        fake_backend_reset();
        dir = TmpDir("failure");
        CleanDir(dir);
        WriteConfig(dir, "{\"power_on_jitter_max_ms\":50}");
    }
    void TearDown() override { CleanDir(dir); }
};

static device_thread_t *failing_thread_create(void (*entry)(void *user), void *user)
{
    (void)entry;
    (void)user;
    return nullptr; /* 模拟线程创建失败 */
}

} // namespace

TEST_F(FailureTest, BackendInitFailRollsBack)
{
    fake_backend_set_init_fail(1);
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = nullptr;
    device_error_t e;
    device_result_t rc = device_host_create(&o, &h, &e);
    /* DEMO_ERR 经 net_ctx 原样返回 → facade 映射 BACKEND_INIT */
    EXPECT_EQ(rc, DEVICE_ERR_BACKEND_INIT);
    EXPECT_EQ(h, nullptr);

    /* 无残留：随后可正常创建 */
    fake_backend_set_init_fail(0);
    h = CreateHost(o);
    DestroyHost(&h);
}

TEST_F(FailureTest, BackendFactoryErrorMaps)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.backend = DEVICE_BACKEND_ESP32C2;
    device_host_t *h = nullptr;
    device_error_t e;
    EXPECT_EQ(device_host_create(&o, &h, &e), DEVICE_ERR_NOT_SUPPORTED);
    EXPECT_EQ(h, nullptr);
    /* 后端创建失败不经过 net_ctx：无 init 调用 */
    EXPECT_EQ(fake_backend_deinit_calls(), 0);
}

TEST_F(FailureTest, ThreadCreateFailKeepsCreated)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;

    device_platform_t bad = device_platform_default;
    bad.thread_create = failing_thread_create;
    EXPECT_EQ(device_host_start_internal(h, &e, &bad), DEVICE_ERR_THREAD);

    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_CREATED); /* 线程创建失败保持 CREATED */

    /* 随后正常 start 仍可运行 */
    EXPECT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}

TEST_F(FailureTest, EventsUnwritableFailsCreate)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    std::string blocker = dir + "/blocker.txt";
    WriteFile(blocker, "x");
    std::string events_path = blocker + "/events.jsonl";
    o.events_jsonl_path = events_path.c_str(); /* 父级是文件 → 无法打开 */
    device_host_t *h = nullptr;
    device_error_t e;
    EXPECT_EQ(device_host_create(&o, &h, &e), DEVICE_ERR_IO);
    EXPECT_EQ(h, nullptr);
}

TEST_F(FailureTest, ConfigInvalidFailsCreate)
{
    WriteFile(dir + "/bad.json", "{invalid json");
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    cfg_path = dir + "/bad.json";
    o.config_path = cfg_path.c_str();
    device_host_t *h = nullptr;
    device_error_t e;
    EXPECT_EQ(device_host_create(&o, &h, &e), DEVICE_ERR_CONFIG_INVALID);
    EXPECT_EQ(h, nullptr);
}
