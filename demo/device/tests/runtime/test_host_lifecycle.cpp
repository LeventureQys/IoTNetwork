#include "test_host_fixture.h"

#include <cstring>

namespace {

struct LifecycleTest : ::testing::Test {
    std::string dir;
    void SetUp() override
    {
        fake_backend_reset();
        dir = TmpDir("lifecycle");
        CleanDir(dir);
        WriteConfig(dir, "{\"power_on_jitter_max_ms\":50}");
    }
    void TearDown() override { CleanDir(dir); }
};

} // namespace

TEST_F(LifecycleTest, CreateDefaults)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_CREATED);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, CreateNullArgs)
{
    device_error_t e;
    device_host_t *h = (device_host_t *)(uintptr_t)1;
    EXPECT_EQ(device_host_create(nullptr, &h, &e), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(h, nullptr);
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    EXPECT_EQ(device_host_create(&o, nullptr, &e), DEVICE_ERR_INVALID_ARGUMENT);
}

TEST_F(LifecycleTest, CreateBadIndex)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.device_index = 16;
    device_host_t *h = nullptr;
    device_error_t e;
    EXPECT_EQ(device_host_create(&o, &h, &e), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(h, nullptr);
}

TEST_F(LifecycleTest, CreateBadBackend)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.backend = (device_backend_kind_t)99;
    device_host_t *h = nullptr;
    device_error_t e;
    EXPECT_EQ(device_host_create(&o, &h, &e), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(h, nullptr);
}

TEST_F(LifecycleTest, CreateEsp32NotSupported)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.backend = DEVICE_BACKEND_ESP32C2;
    device_host_t *h = nullptr;
    device_error_t e;
    EXPECT_EQ(device_host_create(&o, &h, &e), DEVICE_ERR_NOT_SUPPORTED);
    EXPECT_EQ(h, nullptr);
}

TEST_F(LifecycleTest, CreateScenarioRequiresSim)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.backend = DEVICE_BACKEND_LINUX;
    o.scenario_path = "scenario.json";
    device_host_t *h = nullptr;
    device_error_t e;
    EXPECT_EQ(device_host_create(&o, &h, &e), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(h, nullptr);
}

#ifdef _WIN32
TEST_F(LifecycleTest, CreateLinuxOnWindowsNotSupported)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.backend = DEVICE_BACKEND_LINUX;
    device_host_t *h = nullptr;
    device_error_t e;
    EXPECT_EQ(device_host_create(&o, &h, &e), DEVICE_ERR_NOT_SUPPORTED);
    EXPECT_EQ(h, nullptr);
}
#endif

TEST_F(LifecycleTest, StartOnceThenInvalid)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    EXPECT_EQ(device_host_start(h, &e), DEVICE_OK);
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_RUNNING);
    EXPECT_EQ(device_host_start(h, &e), DEVICE_ERR_INVALID_STATE);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    EXPECT_EQ(device_host_start(h, &e), DEVICE_ERR_INVALID_STATE);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, RequestStopIdempotent)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_STOPPED);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, RequestStopInCreatedThenStartStillOk)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_CREATED);
    EXPECT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, JoinNeverStarted)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    EXPECT_EQ(device_host_join(h, 50, &e), DEVICE_OK);
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_CREATED);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, JoinTimeoutHostStillValid)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.duration_seconds = 0;
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    /* 未请求停止：50ms 必然超时 */
    EXPECT_EQ(device_host_join(h, 50, &e), DEVICE_ERR_TIMEOUT);
    EXPECT_NE(h, nullptr);
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_RUNNING);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_STOPPED);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, DestroyRunningBusy)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    device_host_t *p = h;
    EXPECT_EQ(device_host_destroy(&p, &e), DEVICE_ERR_BUSY);
    EXPECT_EQ(p, h); /* 句柄保持有效 */
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    p = h;
    EXPECT_EQ(device_host_destroy(&p, &e), DEVICE_OK);
    EXPECT_EQ(p, nullptr);
}

TEST_F(LifecycleTest, DestroyStopRequestedBusy)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    /* 线程可能尚未退出：STOP_REQUESTED 期间 destroy 必须 BUSY（先 join 才是合法顺序） */
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    if (st == DEVICE_HOST_STOP_REQUESTED) {
        device_host_t *p = h;
        EXPECT_EQ(device_host_destroy(&p, &e), DEVICE_ERR_BUSY);
        EXPECT_EQ(p, h);
    }
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, DestroyIdempotent)
{
    device_error_t e;
    EXPECT_EQ(device_host_destroy(nullptr, &e), DEVICE_OK);
    device_host_t *p = nullptr;
    EXPECT_EQ(device_host_destroy(&p, &e), DEVICE_OK);
    EXPECT_EQ(p, nullptr);
}

TEST_F(LifecycleTest, DestroyWithoutStart)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, GetStateNull)
{
    device_host_state_t st;
    EXPECT_EQ(device_host_get_state(nullptr, &st), DEVICE_ERR_INVALID_ARGUMENT);
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    EXPECT_EQ(device_host_get_state(h, nullptr), DEVICE_ERR_INVALID_ARGUMENT);
    DestroyHost(&h);
}

TEST_F(LifecycleTest, CreateDestroyCreateNoResidue)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h1 = CreateHost(o);
    DestroyHost(&h1);
    device_host_t *h2 = CreateHost(o);
    DestroyHost(&h2);
}

TEST_F(LifecycleTest, DurationAutoStop)
{
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.duration_seconds = 1;
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    /* duration 由 C runner 单调时钟实现，无需外部定时器 */
    EXPECT_EQ(device_host_join(h, 10000, &e), DEVICE_OK);
    device_host_state_t st;
    ASSERT_EQ(device_host_get_state(h, &st), DEVICE_OK);
    EXPECT_EQ(st, DEVICE_HOST_STOPPED);
    DestroyHost(&h);
}
