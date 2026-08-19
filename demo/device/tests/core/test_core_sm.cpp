#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>
#include <string>

#include "device_app.h"
#include "device_config.h"
#include "net_abstraction.h"
#include "fake_backend.h"

/* 从根级 demo/tests/test_device_sm.cpp 移植的状态机行为测试
 * （行为冻结：配网/发现/会话/自愈语义不变；测试后端为纯 C 假后端） */

namespace {

struct SmFixture {
    device_config_t cfg;
    device_backend_instance_t inst;
    net_ctx_t *ctx = nullptr;
    device_app_t *app = nullptr;
    std::thread th;

    void Start(const char *nvs_tag, const char *device_id)
    {
        static std::atomic<int> next_port_base{24000};
        fake_backend_reset();
        std::filesystem::create_directories("run");
        device_config_defaults(&cfg);
        cfg.power_on_jitter_max_ms = 50;
        cfg.device_ap_port_base = next_port_base.fetch_add(100);
        snprintf(cfg.nvs_dir, sizeof(cfg.nvs_dir), "run");

        std::string nvs = std::string("run/") + nvs_tag + ".nvs.json";
        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = nvs.c_str();
        opts.target_ssid = cfg.target_ssid;
        opts.target_password = cfg.target_password;
        opts.device_index = 0;
        device_error_t e;
        ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
        ASSERT_EQ(net_ctx_create(inst.vtable, inst.user, nullptr, &ctx), DEMO_OK);
        app = device_app_create(&cfg, ctx, device_id, 0);
        ASSERT_NE(app, nullptr);
        th = std::thread([this] { device_app_run(app); });
    }

    void Stop()
    {
        if (app) {
            device_app_request_stop(app);
            if (th.joinable())
                th.join();
            device_app_destroy(app);
            app = nullptr;
        }
        if (ctx) {
            net_ctx_destroy(ctx);
            ctx = nullptr;
        }
        device_backend_instance_destroy(&inst);
    }

    bool WaitState(device_state_t s, int timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (device_app_get_state(app) == s)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
};

} // namespace

TEST(CoreSm, NoCredsGoesToProvision)
{
    std::filesystem::remove("run/dev0.nvs.json");
    SmFixture f;
    f.Start("dev0", "02:00:00:00:00:01");
    EXPECT_TRUE(f.WaitState(DEV_STATE_AP_PROVISION, 5000));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (fake_backend_ap_started())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_STREQ(f.app->ap_ssid, "Modu_0001");
    EXPECT_TRUE(fake_backend_ap_started());
    f.Stop();
}

TEST(CoreSm, CredsJoinTargetWifi)
{
    std::filesystem::remove("run/dev1.nvs.json");
    {
        device_config_t params;
        device_config_defaults(&params);
        snprintf(params.nvs_dir, sizeof(params.nvs_dir), "run");
        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = "run/dev1.nvs.json";
        opts.target_ssid = params.target_ssid;
        opts.target_password = params.target_password;
        device_backend_instance_t inst;
        device_error_t e;
        ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
        net_ctx_t *c = nullptr;
        ASSERT_EQ(net_ctx_create(inst.vtable, inst.user, nullptr, &c), DEMO_OK);
        const char *creds = "{\"schema\":1,\"creds\":[{\"ssid\":\"TactileFactory-2.4G\","
                            "\"password\":\"securepass123\",\"confirmed\":1}]}";
        EXPECT_EQ(net_nvs_set(c, "wifi_creds", (const uint8_t *)creds,
                              (int)strlen(creds)),
                  DEMO_OK);
        net_ctx_destroy(c);
        device_backend_instance_destroy(&inst);
    }
    SmFixture f;
    f.Start("dev1", "02:00:00:00:00:02");
    EXPECT_TRUE(f.WaitState(DEV_STATE_DISCOVERY, 8000));
    f.Stop();
}

TEST(CoreSm, AuthFailEntersProvision)
{
    std::filesystem::remove("run/dev2.nvs.json");
    {
        device_config_t params;
        device_config_defaults(&params);
        snprintf(params.nvs_dir, sizeof(params.nvs_dir), "run");
        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = "run/dev2.nvs.json";
        opts.target_ssid = params.target_ssid;
        opts.target_password = params.target_password;
        device_backend_instance_t inst;
        device_error_t e;
        ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
        net_ctx_t *c = nullptr;
        ASSERT_EQ(net_ctx_create(inst.vtable, inst.user, nullptr, &c), DEMO_OK);
        const char *creds = "{\"schema\":1,\"creds\":[{\"ssid\":\"TactileFactory-2.4G\","
                            "\"password\":\"wrongpass\",\"confirmed\":1}]}";
        net_nvs_set(c, "wifi_creds", (const uint8_t *)creds, (int)strlen(creds));
        net_ctx_destroy(c);
        device_backend_instance_destroy(&inst);
    }
    fake_backend_set_auth_fail(1);
    SmFixture f;
    f.Start("dev2", "02:00:00:00:00:03");
    EXPECT_TRUE(f.WaitState(DEV_STATE_AP_PROVISION, 8000));
    f.Stop();
}

TEST(CoreSm, CredentialRollback)
{
    std::filesystem::remove("run/dev3.nvs.json");
    {
        device_config_t params;
        device_config_defaults(&params);
        snprintf(params.nvs_dir, sizeof(params.nvs_dir), "run");
        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = "run/dev3.nvs.json";
        opts.target_ssid = params.target_ssid;
        opts.target_password = params.target_password;
        device_backend_instance_t inst;
        device_error_t e;
        ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
        net_ctx_t *c = nullptr;
        ASSERT_EQ(net_ctx_create(inst.vtable, inst.user, nullptr, &c), DEMO_OK);
        const char *creds = "{\"schema\":1,\"creds\":[{\"ssid\":\"NewWifi\","
                            "\"password\":\"newpass123\",\"confirmed\":0},"
                            "{\"ssid\":\"OldWifi\",\"password\":\"oldpass123\","
                            "\"confirmed\":1}]}";
        net_nvs_set(c, "wifi_creds", (const uint8_t *)creds, (int)strlen(creds));
        net_ctx_destroy(c);
        device_backend_instance_destroy(&inst);
    }
    SmFixture f;
    f.Start("dev3", "02:00:00:00:00:04");
    EXPECT_TRUE(f.WaitState(DEV_STATE_STA_JOIN, 5000) ||
                f.WaitState(DEV_STATE_AP_PROVISION, 5000));
    f.Stop();
    {
        device_config_t params;
        device_config_defaults(&params);
        snprintf(params.nvs_dir, sizeof(params.nvs_dir), "run");
        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = "run/dev3.nvs.json";
        opts.target_ssid = params.target_ssid;
        opts.target_password = params.target_password;
        device_backend_instance_t inst;
        device_error_t e;
        ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
        net_ctx_t *c = nullptr;
        ASSERT_EQ(net_ctx_create(inst.vtable, inst.user, nullptr, &c), DEMO_OK);
        uint8_t buf[1024];
        int len = (int)sizeof(buf);
        EXPECT_EQ(net_nvs_get(c, "wifi_creds", buf, &len), DEMO_OK);
        std::string s((const char *)buf, (size_t)len);
        EXPECT_NE(s.find("OldWifi"), std::string::npos);
        EXPECT_EQ(s.find("NewWifi"), std::string::npos);
        net_ctx_destroy(c);
        device_backend_instance_destroy(&inst);
    }
}

TEST(CoreSm, LoneUnconfirmedCredentialReopensProvisionAp)
{
    const char *tag = "dev4";
    std::filesystem::remove("run/dev4.nvs.json");
    {
        device_config_t params;
        device_config_defaults(&params);
        snprintf(params.nvs_dir, sizeof(params.nvs_dir), "run");
        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = "run/dev4.nvs.json";
        opts.target_ssid = params.target_ssid;
        opts.target_password = params.target_password;
        device_backend_instance_t inst;
        device_error_t e;
        ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
        net_ctx_t *c = nullptr;
        ASSERT_EQ(net_ctx_create(inst.vtable, inst.user, nullptr, &c), DEMO_OK);
        const char *creds = "{\"schema\":1,\"creds\":[{\"ssid\":\"WrongButReachable\","
                            "\"password\":\"securepass123\",\"confirmed\":0}]}";
        ASSERT_EQ(net_nvs_set(c, "wifi_creds", (const uint8_t *)creds,
                              (int)strlen(creds)),
                  DEMO_OK);
        net_ctx_destroy(c);
        device_backend_instance_destroy(&inst);
    }

    SmFixture fixture;
    fixture.Start(tag, "02:00:00:00:00:05");
    EXPECT_TRUE(fixture.WaitState(DEV_STATE_AP_PROVISION, 5000));
    fixture.Stop();

    device_config_t params;
    device_config_defaults(&params);
    snprintf(params.nvs_dir, sizeof(params.nvs_dir), "run");
    device_sim_backend_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.nvs_file = "run/dev4.nvs.json";
    opts.target_ssid = params.target_ssid;
    opts.target_password = params.target_password;
    device_backend_instance_t inst;
    device_error_t e;
    ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
    net_ctx_t *c = nullptr;
    ASSERT_EQ(net_ctx_create(inst.vtable, inst.user, nullptr, &c), DEMO_OK);
    uint8_t buf[1024];
    int len = (int)sizeof(buf);
    EXPECT_EQ(net_nvs_get(c, "wifi_creds", buf, &len), DEMO_ERR); /* 已被清除 */
    net_ctx_destroy(c);
    device_backend_instance_destroy(&inst);
}

TEST(CoreSm, TxTextValidatesState)
{
    SmFixture f;
    f.Start("dev5", "02:00:00:00:00:06");
    EXPECT_EQ(device_app_tx_text(f.app, "hello"), DEMO_ERR); /* 非会话态 */
    f.Stop();
}
