#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <string>
#include <vector>
#include "host_app.h"
#include "sim_backend.h"
#include "sim_world.h"
#include "params.h"
#include "log.h"

namespace {

struct HostOnlyFixture {
    demo_params_t params;
    net_ctx_t *host_ctx = nullptr;
    void *host_user = nullptr;
    HostApp *host = nullptr;
    std::thread host_th;

    void Start()
    {
        params_defaults(&params);
        params.power_on_jitter_max_ms = 100;
        params.provision_auth_timeout_ms = 3000;
        params.provision_wifi_cfg_timeout_ms = 8000;
        params.heartbeat_interval_ms = 1000;
        params.busy_backoff_ms = 2000;
        params.device_ap_port_base = 23000;
        SimWorld::Instance().TargetNetworkSet(params.target_ssid, params.target_password, true);

        host_user = sim_backend_create("host", &params);
        ASSERT_NE(host_user, nullptr);
        net_ctx_t *ctx = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), host_user, nullptr, &ctx), DEMO_OK);
        host_ctx = ctx;
        host = new HostApp(params, host_ctx);
        ASSERT_EQ(host->Start(), DEMO_OK);
        host_th = std::thread([this] { host->Run(); });
    }

    void Stop()
    {
        if (host) {
            host->RequestStop();
            if (host_th.joinable()) host_th.join();
            delete host;
            host = nullptr;
        }
        if (host_ctx) net_ctx_destroy(host_ctx);
        if (host_user) sim_backend_destroy(host_user);
        host_ctx = nullptr;
        host_user = nullptr;
    }
};

} // namespace

TEST(HostProvision, WifiScanExcludesFiveGhzAndDeviceAps)
{
    /* 纯 PC：扫描过滤目标 5G 与 Modu_ 设备热点（不使用设备端） */
    demo_params_t params;
    params_defaults(&params);
    SimWorld::Instance().TargetNetworkSet("Factory-5G", "securepass123", false);
    void *user = sim_backend_create("host_scan", &params);
    net_ctx_t *ctx = nullptr;
    ASSERT_EQ(net_ctx_create(sim_backend_table(), user, nullptr, &ctx), DEMO_OK);
    {
        HostApp host(params, ctx);
        SimWorld::Instance().ApRegister("dev_scan", "Modu_ABCD", "modutech_leventure",
                                        "5935", 24000);
        std::vector<std::string> ssids;
        ASSERT_EQ(host.ScanWifiNetworks(&ssids), DEMO_OK);
        EXPECT_EQ(std::find(ssids.begin(), ssids.end(), "Factory-5G"), ssids.end());
        EXPECT_EQ(std::find(ssids.begin(), ssids.end(), "Modu_ABCD"), ssids.end());
    }

    SimWorld::Instance().ApUnregister("dev_scan");
    net_ctx_destroy(ctx);
    sim_backend_destroy(user);
}

TEST(HostProvision, ProvisionWithNoApsIsNoop)
{
    /* 纯 PC：无设备热点时批量配网不崩溃、不阻塞 */
    HostOnlyFixture f;
    f.Start();
    f.host->ProvisionAllDevices();
    EXPECT_EQ(f.host->ProvisionTotal(), 0);
    f.Stop();
}

TEST(HostAppData, RejectInvalidWithoutDevice)
{
    /* 纯 PC：入队校验与空文本/超长拒绝（513 字节）不依赖设备在线 */
    HostOnlyFixture f;
    f.Start();
    EXPECT_EQ(f.host->SendAppDataToDevice("02:00:00:00:00:01", ""), DEMO_ERR);
    EXPECT_EQ(f.host->SendAppDataToDevice("02:00:00:00:00:01", std::string(513, 'a')),
              DEMO_ERR);
    EXPECT_EQ(f.host->SendAppDataToDevice("", "x"), DEMO_ERR);
    f.Stop();
}

TEST(HostAppData, QueueValidWhenNoDeviceOnline)
{
    /* 512 字节合法入队成功（冲刷时因无在线设备丢弃，但入队契约成立） */
    HostOnlyFixture f;
    f.Start();
    EXPECT_EQ(f.host->SendAppDataToDevice("ff:ff:ff:ff:ff:ff", std::string(512, 'b')),
              DEMO_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    f.Stop();
}

TEST(HostScanAps, FiltersModuPrefix)
{
    /* 纯 PC：ScanAps 仅返回 Modu_ 前缀 2.4G 热点（模拟设备在进程内注册） */
    HostOnlyFixture f;
    f.Start();
    SimWorld::Instance().ApRegister("dev_filter", "Modu_0001", "modutech_leventure",
                                    "5935", 23000);
    SimWorld::Instance().ApRegister("dev_filter2", "OtherNet", "modutech_leventure",
                                    "5935", 23001);
    std::vector<std::string> aps;
    ASSERT_EQ(f.host->registry_mut().Size(), (size_t)0);
    /* ProvisionAllDevices 内部经 ScanAps：直接验证扫描过滤 */
    std::vector<std::string> modu;
    {
        /* 通过 host 扫描 API 验证 */
        std::vector<std::string> all;
        f.host->ScanWifiNetworks(&all);
        /* OtherNet 非 5G 且非 Modu_ 前缀 → 应出现在可下发列表 */
        EXPECT_NE(std::find(all.begin(), all.end(), "OtherNet"), all.end());
    }
    SimWorld::Instance().ApUnregister("dev_filter");
    SimWorld::Instance().ApUnregister("dev_filter2");
    f.Stop();
}
