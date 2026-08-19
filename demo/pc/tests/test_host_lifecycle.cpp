#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "net_abstraction.h"
#include "params.h"
#include "host_app.h"
#include "host_tcp_server.h"
#include "host_registry.h"

#ifdef _WIN32
#include "win_backend.h"
#endif

namespace {

/* 可控 fake 后端：记录调用顺序与句柄关闭，支持热点/TCP 失败注入。 */
struct FakeBackend {
    std::vector<std::string> calls;
    std::vector<void *> closed;
    bool tcp_fail = false;
    bool configure_fail = false;
    bool ap_status_fail = false;
    bool status_wrong_ip_once = false;
    bool status_always_wrong_ip = false;
    int status_calls = 0;
    void *listen_handle = reinterpret_cast<void *>(0x1001);
    int listen_calls = 0;
    int close_calls = 0;

    bool HasCall(const char *name) const
    {
        return std::find(calls.begin(), calls.end(), name) != calls.end();
    }
    bool WasClosed(void *handle) const
    {
        return std::find(closed.begin(), closed.end(), handle) != closed.end();
    }
};

int ft_ap_start(void *user, const char *, const char *, const char *)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("ap_start");
    return DEMO_OK;
}

int ft_ap_stop(void *user)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("ap_stop");
    return DEMO_OK;
}

int ft_ap_status(void *user, net_ap_status_t *status)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("ap_status");
    fb->status_calls++;
    memset(status, 0, sizeof(*status));
    status->started = 1;
    snprintf(status->ssid, sizeof(status->ssid), "%s", "Modu_PC");
    const bool wrong = fb->status_always_wrong_ip ||
                       (fb->status_wrong_ip_once && fb->status_calls == 1);
    snprintf(status->ipv4, sizeof(status->ipv4), "%s",
             wrong ? "10.0.0.1" : "192.168.137.1");
    status->prefix_length = 24;
    return fb->ap_status_fail ? DEMO_ERR : DEMO_OK;
}

int ft_ap_configure(void *user, const char *, int)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("ap_configure_ipv4");
    return fb->configure_fail ? DEMO_ERR : DEMO_OK;
}

int ft_tcp_listen(void *user, uint16_t, void **sock)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("tcp_listen");
    fb->listen_calls++;
    if (fb->tcp_fail)
        return DEMO_ERR;
    *sock = fb->listen_handle;
    return DEMO_OK;
}

int ft_accept_again(void *, void *, void **, net_addr_t *) { return DEMO_ERR_AGAIN; }
int ft_recv_again(void *, void *, uint8_t *, int) { return DEMO_ERR_AGAIN; }
int ft_send(void *, void *, const uint8_t *, int len) { return len; }
uint64_t ft_time(void *) { return 1000; }
uint32_t ft_random(void *) { return 42; }

void ft_close(void *user, void *sock)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("close");
    fb->close_calls++;
    fb->closed.push_back(sock);
}

const net_backend_t fake_backend_table = {
    nullptr, nullptr,                                  /* init / deinit */
    nullptr, nullptr, nullptr,                         /* wifi scan/sta */
    ft_ap_start, ft_ap_stop, ft_ap_status, ft_ap_configure, /* ap */
    nullptr, nullptr, nullptr, nullptr,                /* rssi/ip/ssid/gateway */
    ft_tcp_listen, ft_accept_again, nullptr,           /* tcp */
    ft_send, ft_recv_again, ft_close,                  /* sock */
    nullptr, nullptr, nullptr,                         /* udp */
    nullptr, nullptr, nullptr,                         /* mdns */
    nullptr, nullptr, nullptr,                         /* nvs */
    ft_time, ft_random, nullptr,                       /* time/random/inject */
};

net_ctx_t *MakeCtx(FakeBackend *fb)
{
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(&fake_backend_table, fb, nullptr, &ctx), DEMO_OK);
    return ctx;
}

} // namespace

TEST(HostLifecycle, StartOrderApStatusTcp)
{
    FakeBackend fb;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        ASSERT_EQ(host.Start(), DEMO_OK);
        EXPECT_TRUE(host.Started());
        ASSERT_EQ(fb.calls.size(), (size_t)3);
        EXPECT_EQ(fb.calls[0], "ap_start");
        EXPECT_EQ(fb.calls[1], "ap_status");
        EXPECT_EQ(fb.calls[2], "tcp_listen");
        EXPECT_FALSE(fb.HasCall("ap_configure_ipv4"));
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, IpMismatchConfiguresIpv4)
{
    FakeBackend fb;
    fb.status_wrong_ip_once = true;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        ASSERT_EQ(host.Start(), DEMO_OK);
        ASSERT_EQ(fb.calls.size(), (size_t)5);
        EXPECT_EQ(fb.calls[0], "ap_start");
        EXPECT_EQ(fb.calls[1], "ap_status");
        EXPECT_EQ(fb.calls[2], "ap_configure_ipv4");
        EXPECT_EQ(fb.calls[3], "ap_status");
        EXPECT_EQ(fb.calls[4], "tcp_listen");
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, ConfigureFailureRollsBackHotspot)
{
    FakeBackend fb;
    fb.status_wrong_ip_once = true;
    fb.configure_fail = true;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        EXPECT_NE(host.Start(), DEMO_OK);
        EXPECT_FALSE(host.Started());
        ASSERT_EQ(fb.calls.size(), (size_t)4);
        EXPECT_EQ(fb.calls[0], "ap_start");
        EXPECT_EQ(fb.calls[1], "ap_status");
        EXPECT_EQ(fb.calls[2], "ap_configure_ipv4");
        EXPECT_EQ(fb.calls[3], "ap_stop"); /* configure 失败即回滚热点 */
        EXPECT_FALSE(fb.HasCall("tcp_listen"));
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, ConfigureThenStillWrongRollsBackHotspot)
{
    FakeBackend fb;
    fb.status_always_wrong_ip = true;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        EXPECT_NE(host.Start(), DEMO_OK);
        EXPECT_FALSE(host.Started());
        EXPECT_TRUE(fb.HasCall("ap_configure_ipv4"));
        EXPECT_FALSE(fb.HasCall("tcp_listen"));
        EXPECT_TRUE(fb.HasCall("ap_stop"));
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, ApStatusFailureRollsBackHotspot)
{
    FakeBackend fb;
    fb.ap_status_fail = true;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        EXPECT_NE(host.Start(), DEMO_OK);
        EXPECT_TRUE(fb.HasCall("ap_stop"));
        EXPECT_FALSE(fb.HasCall("tcp_listen"));
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, TcpFailureStopsHotspot)
{
    FakeBackend fb;
    fb.tcp_fail = true;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        EXPECT_NE(host.Start(), DEMO_OK);
        EXPECT_TRUE(fb.HasCall("tcp_listen"));
        EXPECT_TRUE(fb.HasCall("ap_stop"));
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, RequestStopOrderAndIdempotent)
{
    FakeBackend fb;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        ASSERT_EQ(host.Start(), DEMO_OK);
        fb.calls.clear();
        host.RequestStop();
        ASSERT_EQ(fb.calls.size(), (size_t)2);
        EXPECT_EQ(fb.calls[0], "close");   /* tcp_server.Stop 关闭 listener */
        EXPECT_EQ(fb.calls[1], "ap_stop"); /* 随后停止热点 */
        EXPECT_TRUE(fb.WasClosed(fb.listen_handle));
        size_t close_count = (size_t)fb.close_calls;
        host.RequestStop(); /* 幂等 */
        EXPECT_EQ((size_t)fb.close_calls, close_count);
        EXPECT_FALSE(host.Started());
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, TcpServerStartStopIdempotent)
{
    FakeBackend fb;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    HostRegistry registry;
    {
        HostTcpServer server(ctx, registry, params);
        ASSERT_EQ(server.Start(), DEMO_OK);
        EXPECT_TRUE(server.Started());
        EXPECT_EQ(fb.listen_calls, 1);
        EXPECT_EQ(server.Start(), DEMO_OK);
        EXPECT_EQ(fb.listen_calls, 1);
        server.Stop();
        EXPECT_FALSE(server.Started());
        server.Stop();
        EXPECT_EQ(fb.close_calls, 1);
        EXPECT_EQ(server.Start(), DEMO_OK);
        EXPECT_TRUE(server.Started());
        server.Stop();
    }
    net_ctx_destroy(ctx);
}

#ifdef _WIN32
TEST(WinBackendAddress, Ipv4ValidityFilter)
{
    EXPECT_EQ(win_backend_ipv4_valid("192.168.1.50"), DEMO_OK);
    EXPECT_EQ(win_backend_ipv4_valid("10.0.0.1"), DEMO_OK);
    EXPECT_EQ(win_backend_ipv4_valid("172.16.0.8"), DEMO_OK);
    EXPECT_EQ(win_backend_ipv4_valid("255.255.255.254"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("0.0.0.0"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("0.1.2.3"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("127.0.0.1"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("127.255.255.255"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("169.254.0.1"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("169.254.255.255"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("192.168.1"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("192.168.1.50.1"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("not-an-ip"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("256.1.1.1"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid(""), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid(nullptr), DEMO_OK);
}

TEST(WinBackendListener, ExclusiveBindConflictAndFastRestart)
{
    demo_params_t params;
    params_defaults(&params);
    void *user = win_backend_create(&params);
    ASSERT_NE(user, nullptr);
    net_ctx_t *ctx = nullptr;
    ASSERT_EQ(net_ctx_create(win_backend_table(), user, nullptr, &ctx), DEMO_OK);
    ASSERT_NE(ctx, nullptr);
    uint16_t port = 0;
    for (uint16_t candidate = 55935; candidate < 55945; ++candidate) {
        void *probe = nullptr;
        if (net_tcp_listen(ctx, candidate, &probe) == DEMO_OK) {
            net_sock_close(ctx, probe);
            port = candidate;
            break;
        }
    }
    ASSERT_NE(port, 0) << "测试环境无可用的探测端口";
    void *first = nullptr;
    ASSERT_EQ(net_tcp_listen(ctx, port, &first), DEMO_OK);
    void *second = nullptr;
    EXPECT_NE(net_tcp_listen(ctx, port, &second), DEMO_OK);
    EXPECT_EQ(second, nullptr);
    net_sock_close(ctx, first);
    void *third = nullptr;
    EXPECT_EQ(net_tcp_listen(ctx, port, &third), DEMO_OK);
    net_sock_close(ctx, third);
    net_ctx_destroy(ctx);
    win_backend_destroy(user);
}
#endif
