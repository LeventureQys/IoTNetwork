#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <string>

#include "device_app.h"
#include "device_eventlog.h"
#include "device_config.h"
#include "net_abstraction.h"
#include "fake_backend.h"

/* 从根级 demo/tests/test_device_logic.cpp 移植的核心行为测试
 * （被测生产目标由 C 编译器编译；测试驱动为 C++ gtest + 测试专用纯 C 假后端） */

namespace {

struct CoreFixture {
    device_config_t cfg;
    device_backend_instance_t inst;
    net_ctx_t *ctx = nullptr;
    device_app_t *app = nullptr;
    std::string nvs_path;

    void SetUp(const char *nvs_tag = "dev_logic", const char *device_id = "02:00:00:00:00:01",
               bool fresh = true)
    {
        fake_backend_reset();
        std::filesystem::create_directories("run");
        device_config_defaults(&cfg);
        snprintf(cfg.nvs_dir, sizeof(cfg.nvs_dir), "run");
        nvs_path = std::string("run/") + nvs_tag + ".nvs.json";
        if (fresh)
            std::filesystem::remove(nvs_path);

        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = nvs_path.c_str();
        opts.device_index = 0;
        device_error_t e;
        ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);

        int rc = net_ctx_create(inst.vtable, inst.user, nullptr, &ctx);
        ASSERT_EQ(rc, DEMO_OK);
        app = device_app_create(&cfg, ctx, device_id, 0);
        ASSERT_NE(app, nullptr);
    }

    void TearDown()
    {
        if (app)
            device_app_destroy(app);
        if (ctx)
            net_ctx_destroy(ctx);
        device_backend_instance_destroy(&inst);
        app = nullptr;
        ctx = nullptr;
    }
};

} // namespace

TEST(CoreEventLog, RecordAndFill)
{
    CoreFixture f;
    f.SetUp();
    evlog_record(f.app, "power on");
    evlog_record(f.app, "wifi connected: %s", "TestWifi");
    char out[512];
    EXPECT_EQ(evlog_fill_report(f.app, out, sizeof(out)), DEMO_OK);
    EXPECT_NE(strstr(out, "power on"), nullptr);
    EXPECT_NE(strstr(out, "wifi connected"), nullptr);
    f.TearDown();
}

TEST(CoreEventLog, RingWrap50)
{
    CoreFixture f;
    f.SetUp();
    for (int i = 0; i < 60; i++)
        evlog_record(f.app, "event %d", i);
    char out[4096];
    evlog_fill_report(f.app, out, sizeof(out));
    EXPECT_NE(strstr(out, "event 59"), nullptr);
    EXPECT_EQ(strstr(out, "event 0"), nullptr); /* 已被覆盖 */
    f.TearDown();
}

TEST(CoreEventLog, NvsPersist)
{
    {
        CoreFixture f;
        f.SetUp("dev_event_persist");
        evlog_record(f.app, "persist-check");
        f.TearDown();
    }
    {
        CoreFixture f;
        f.SetUp("dev_event_persist", "02:00:00:00:00:01", false);
        char out[512];
        evlog_fill_report(f.app, out, sizeof(out));
        EXPECT_NE(strstr(out, "persist-check"), nullptr);
        f.TearDown();
    }
}
