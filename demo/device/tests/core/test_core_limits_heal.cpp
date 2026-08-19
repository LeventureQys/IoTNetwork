#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <string>

#include "device_app.h"
#include "device_eventlog.h"
#include "device_limits.h"
#include "device_heal.h"
#include "device_config.h"
#include "net_abstraction.h"
#include "fake_backend.h"

/* 从根级 demo/tests/test_device_logic.cpp 移植：限速与自愈退避 */

namespace {

struct CoreFixture {
    device_config_t cfg;
    device_backend_instance_t inst;
    net_ctx_t *ctx = nullptr;
    device_app_t *app = nullptr;

    void SetUp()
    {
        fake_backend_reset();
        std::filesystem::create_directories("run");
        device_config_defaults(&cfg);
        snprintf(cfg.nvs_dir, sizeof(cfg.nvs_dir), "run");
        std::filesystem::remove("run/dev_logic2.nvs.json");
        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = "run/dev_logic2.nvs.json";
        opts.target_ssid = cfg.target_ssid;
        opts.target_password = cfg.target_password;
        device_error_t e;
        ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
        ASSERT_EQ(net_ctx_create(inst.vtable, inst.user, nullptr, &ctx), DEMO_OK);
        app = device_app_create(&cfg, ctx, "02:00:00:00:00:01", 0);
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

TEST(CoreLimits, BurstInject)
{
    CoreFixture f;
    f.SetUp();
    EXPECT_EQ(limits_allow_send(f.app, 0), 1);
    EXPECT_EQ(limits_allow_send(f.app, 1), 1);
    f.app->rate_burst_flag = 1;
    EXPECT_EQ(limits_allow_send(f.app, 0), 0);
    EXPECT_EQ(f.app->rate_burst_flag, 0);
    f.TearDown();
}

TEST(CoreHeal, BackoffSequence)
{
    CoreFixture f;
    f.SetUp();
    f.cfg.reconnect_backoff_base_ms = 1000;
    f.cfg.reconnect_backoff_cap_ms = 5000;
    f.cfg.reconnect_backoff_jitter_ms = 0;
    EXPECT_EQ(heal_next_backoff_ms(f.app), 1000);
    EXPECT_EQ(heal_next_backoff_ms(f.app), 2000);
    EXPECT_EQ(heal_next_backoff_ms(f.app), 4000);
    EXPECT_EQ(heal_next_backoff_ms(f.app), 5000);
    EXPECT_EQ(heal_next_backoff_ms(f.app), 5000);
    heal_reset_backoff(f.app);
    EXPECT_EQ(heal_next_backoff_ms(f.app), 1000);
    f.TearDown();
}

TEST(CoreHeal, BusyBackoffPending)
{
    CoreFixture f;
    f.SetUp();
    f.cfg.busy_backoff_ms = 777;
    f.app->busy_pending = 1;
    EXPECT_EQ(heal_next_backoff_ms(f.app), 777);
    EXPECT_EQ(f.app->busy_pending, 0); /* 一次性消费 */
    f.TearDown();
}
