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

/* beta v1.1 六态状态机测试（BOOT→WIFI_SCAN→STA_JOIN→CONNECT→SESSION→HEAL）。
 * 使用纯 C 假后端：扫描返回 pc_ap_ssid、STA 验证固定密码、TCP 直连固定
 * 192.168.137.1:5935（网络字节序 0x0189A8C0 / 0x2F17）。 */

namespace {

/* 192.168.137.1 / 5935 的网络字节序（与 sim_world/linux_wifi 约定一致：
 * 小端主机内存字节序即点分四段顺序；5935=0x172F → 网络序 0x2F17） */
static const uint32_t kPcIpNet = 0x0189A8C0u;
static const uint16_t kPcPortNet = 0x2F17u;

struct SmFixture {
    device_config_t cfg;
    device_backend_instance_t inst;
    net_ctx_t *ctx = nullptr;
    device_app_t *app = nullptr;
    std::thread th;

    /* 注意：不在此处 reset fake_backend——测试先 reset 再设置开关，再调用 Start。 */
    void Start(const char *nvs_tag, const char *device_id,
               int heartbeat_interval_ms = 10000)
    {
        std::filesystem::create_directories("run");
        device_config_defaults(&cfg);
        cfg.power_on_jitter_max_ms = 20;
        cfg.wifi_backoff_base_ms = 20;
        cfg.wifi_backoff_cap_ms = 200;
        cfg.wifi_backoff_jitter_ms = 0;
        cfg.reconnect_backoff_base_ms = 20;
        cfg.reconnect_backoff_cap_ms = 200;
        cfg.reconnect_backoff_jitter_ms = 0;
        cfg.heartbeat_interval_ms = heartbeat_interval_ms;
        cfg.heartbeat_dead_ms = 0;
        cfg.busy_backoff_ms = 500;
        snprintf(cfg.nvs_dir, sizeof(cfg.nvs_dir), "run");

        std::string nvs = std::string("run/") + nvs_tag + ".nvs.json";
        std::filesystem::remove(nvs);
        device_sim_backend_options_t opts;
        memset(&opts, 0, sizeof(opts));
        opts.nvs_file = nvs.c_str();
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
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    /* 在整个窗口内状态都未达到 s（用于断言"绝不进入某状态"） */
    bool NeverReached(device_state_t s, int window_ms)
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(window_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (device_app_get_state(app) == s)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    bool WaitFor(bool (*pred)(void *), void *user, int timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred(user))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
};

static bool AckOkPred(void *p)
{
    device_app_t *a = (device_app_t *)p;
    return a->session_ack_ok == 1 && strcmp(a->session_id, "deadbeef") == 0;
}

static bool BusyHealPred(void *p)
{
    device_app_t *a = (device_app_t *)p;
    return a->state == DEV_STATE_HEAL && a->busy_pending == 0;
}

} // namespace

TEST(CoreSm, ScanNoTargetKeepsScanning)
{
    fake_backend_reset();
    fake_backend_set_no_ap(1);
    SmFixture f;
    f.Start("sm_noap", "02:00:00:00:00:01");
    EXPECT_TRUE(f.WaitState(DEV_STATE_WIFI_SCAN, 3000));
    /* 无目标持续扫描：不得进入 STA_JOIN/SESSION */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(device_app_get_state(f.app), DEV_STATE_WIFI_SCAN);
    EXPECT_EQ(fake_backend_ap_start_calls(), 0);
    f.Stop();
}

TEST(CoreSm, ScanSimilarNotExactNoConnect)
{
    fake_backend_reset();
    fake_backend_set_similar_ssid(1);
    SmFixture f;
    f.Start("sm_similar", "02:00:00:00:00:02");
    EXPECT_TRUE(f.WaitState(DEV_STATE_WIFI_SCAN, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(device_app_get_state(f.app), DEV_STATE_WIFI_SCAN);
    EXPECT_EQ(fake_backend_sta_connected(), 0);
    f.Stop();
}

TEST(CoreSm, ExactTargetSessionFixedTcp)
{
    fake_backend_reset();
    SmFixture f;
    f.Start("sm_happy", "02:00:00:00:00:03");
    EXPECT_TRUE(f.WaitState(DEV_STATE_SESSION, 5000));
    EXPECT_EQ(fake_backend_last_connect_ip(), kPcIpNet);
    EXPECT_EQ(fake_backend_last_connect_port(), kPcPortNet);
    EXPECT_EQ(fake_backend_sta_connected(), 1);
    EXPECT_EQ(fake_backend_ap_start_calls(), 0);
    EXPECT_EQ(fake_backend_mdns_calls(), 0);
    EXPECT_EQ(fake_backend_udp_calls(), 0);
    EXPECT_EQ(fake_backend_tcp_listen_calls(), 0);
    f.Stop();
}

TEST(CoreSm, AuthFailHealRetryNoAp)
{
    fake_backend_reset();
    fake_backend_set_auth_fail(1);
    SmFixture f;
    f.Start("sm_authfail", "02:00:00:00:00:04");
    /* 认证失败：进入 STA_JOIN 尝试，但绝不建立会话，且不启动设备 AP */
    EXPECT_TRUE(f.WaitState(DEV_STATE_STA_JOIN, 3000));
    EXPECT_TRUE(f.NeverReached(DEV_STATE_SESSION, 2000));
    EXPECT_EQ(fake_backend_ap_start_calls(), 0);
    EXPECT_EQ(fake_backend_sta_connected(), 0);
    f.Stop();
}

TEST(CoreSm, DhcpIpZeroDisconnectScan)
{
    fake_backend_reset();
    fake_backend_set_dhcp_fail(1);
    SmFixture f;
    f.Start("sm_dhcp", "02:00:00:00:00:05");
    /* STA 成功但 IP=0 → 断开并回扫描；绝不建立会话 */
    EXPECT_TRUE(f.WaitState(DEV_STATE_WIFI_SCAN, 5000));
    EXPECT_TRUE(f.NeverReached(DEV_STATE_SESSION, 1500));
    EXPECT_GE(fake_backend_sta_disconnect_calls(), 1); /* 发生过断开 */
    EXPECT_EQ(fake_backend_ap_start_calls(), 0);
    f.Stop();
}

TEST(CoreSm, TcpFailWifiHealthyStaysConnectRetry)
{
    fake_backend_reset();
    fake_backend_set_tcp_fail(1);
    SmFixture f;
    f.Start("sm_tcpfail", "02:00:00:00:00:06");
    /* WiFi 健康 → TCP 失败 → HEAL 内按退避重试 CONNECT，不回扫描 */
    EXPECT_TRUE(f.WaitState(DEV_STATE_HEAL, 5000));
    EXPECT_TRUE(f.NeverReached(DEV_STATE_WIFI_SCAN, 1500));
    EXPECT_EQ(fake_backend_last_connect_ip(), kPcIpNet);
    f.Stop();
}

TEST(CoreSm, WifiDropReturnsToScan)
{
    fake_backend_reset();
    SmFixture f;
    f.Start("sm_wifidrop", "02:00:00:00:00:07");
    ASSERT_TRUE(f.WaitState(DEV_STATE_SESSION, 5000));
    fake_backend_set_wifi_drop(1);
    EXPECT_TRUE(f.WaitState(DEV_STATE_WIFI_SCAN, 5000));
    EXPECT_EQ(fake_backend_sta_connected(), 0);
    f.Stop();
}

TEST(CoreSm, HelloAckOkSessionOnline)
{
    fake_backend_reset();
    fake_backend_set_ack_ok(1);
    SmFixture f;
    f.Start("sm_ackok", "02:00:00:00:00:08");
    ASSERT_TRUE(f.WaitState(DEV_STATE_SESSION, 5000));
    EXPECT_TRUE(f.WaitFor(AckOkPred, f.app, 3000));
    f.Stop();
}

TEST(CoreSm, HostAckBusyLongBackoff)
{
    fake_backend_reset();
    fake_backend_set_ack_busy(1);
    SmFixture f;
    f.Start("sm_ackbusy", "02:00:00:00:00:09");
    ASSERT_TRUE(f.WaitState(DEV_STATE_SESSION, 5000));
    /* busy → 断开并进入 HEAL 长退避（busy_backoff_ms=500） */
    EXPECT_TRUE(f.WaitFor(BusyHealPred, f.app, 3000));
    EXPECT_EQ(device_app_get_state(f.app), DEV_STATE_HEAL);
    f.Stop();
}

TEST(CoreSm, HeartbeatTimeoutHeals)
{
    fake_backend_reset();
    SmFixture f;
    f.Start("sm_hb", "02:00:00:00:00:0A", 200);
    ASSERT_TRUE(f.WaitState(DEV_STATE_SESSION, 5000));
    /* 无对端数据 → 心跳超时（200*3/2=300ms）→ HEAL */
    EXPECT_TRUE(f.WaitState(DEV_STATE_HEAL, 5000));
    f.Stop();
}

TEST(CoreSm, TxTextValidatesBoundary)
{
    fake_backend_reset();
    SmFixture f;
    f.Start("sm_tx", "02:00:00:00:00:0B");
    EXPECT_EQ(device_app_tx_text(f.app, "hello"), DEMO_ERR); /* 非会话态 */
    ASSERT_TRUE(f.WaitState(DEV_STATE_SESSION, 5000));

    char big[513];
    memset(big, 'x', 513);
    EXPECT_EQ(device_app_tx_text(f.app, big), DEMO_ERR);       /* >512 */
    EXPECT_EQ(device_app_tx_text(f.app, ""), DEMO_ERR);        /* 空 */
    char ok512[513];
    memset(ok512, 'a', 512);
    ok512[512] = '\0';
    EXPECT_EQ(device_app_tx_text(f.app, ok512), DEMO_OK);      /* 恰好 512 */
    f.Stop();
}
