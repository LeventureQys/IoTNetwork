#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "cJSON.h"
#include "net_abstraction.h"
#include "params.h"
#include "host_app.h"
#include "host_announcer.h"
#include "host_tcp_server.h"
#include "host_registry.h"
#ifdef _WIN32
#include "win_backend.h"
#endif

namespace {

/* 可控 fake 后端：记录调用顺序、句柄关闭，并支持注入 TCP/组播/mDNS 失败 */
struct FakeBackend {
    std::vector<std::string> calls;
    std::vector<std::string> udp_sends; /* 原始报文（host_announce/host_bye JSON） */
    std::vector<void *> closed;
    bool tcp_fail = false;
    bool mcast_fail = false;
    bool mdns_fail = false;
    void *listen_handle = reinterpret_cast<void *>(0x1001);
    void *mcast_handle = reinterpret_cast<void *>(0x1002);
    int listen_calls = 0;
    int mcast_calls = 0;
    int mdns_calls = 0;
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

int ft_err_v(void *) { return DEMO_ERR; }
int ft_err_v2(void *, uint32_t *) { return DEMO_ERR; }
int ft_err_vp(void *, net_ap_info_t *, int *) { return DEMO_ERR; }
int ft_err_vssr(void *, const char *, const char *, wifi_reason_t *) { return DEMO_ERR; }
int ft_err_vss(void *, const char *, const char *, const char *) { return DEMO_ERR; }
int ft_err_vi(void *, int *) { return DEMO_ERR; }
int ft_err_vcb(void *, char *, int) { return DEMO_ERR; }
int ft_err_vcv(void *, const char *, uint8_t *, int *) { return DEMO_ERR; }
int ft_err_vcvn(void *, const char *, const uint8_t *, int) { return DEMO_ERR; }
int ft_err_vc(void *, const char *) { return DEMO_ERR; }
int ft_err_vccc(void *, const char *, const char *) { return DEMO_ERR; }
int ft_err_vcn(void *, const char *, net_mdns_service_t *, int) { return DEMO_ERR; }
int ft_err_vav(void *, const net_addr_t *, void **, int) { return DEMO_ERR; }

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

int ft_mcast_join(void *user, const char *, uint16_t, void **sock)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("mcast_join");
    fb->mcast_calls++;
    if (fb->mcast_fail)
        return DEMO_ERR;
    *sock = fb->mcast_handle;
    return DEMO_OK;
}

int ft_mdns_register(void *user, const net_mdns_service_t *)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("mdns_register");
    fb->mdns_calls++;
    return fb->mdns_fail ? DEMO_ERR : DEMO_OK;
}

int ft_mdns_unregister(void *user, const char *)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("mdns_unregister");
    return DEMO_OK;
}

void ft_close(void *user, void *sock)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("close");
    fb->close_calls++;
    fb->closed.push_back(sock);
}

int ft_udp_send(void *user, const char *, uint16_t, const uint8_t *buf, int len)
{
    FakeBackend *fb = static_cast<FakeBackend *>(user);
    fb->calls.push_back("udp_send");
    fb->udp_sends.emplace_back(reinterpret_cast<const char *>(buf), (size_t)len);
    return DEMO_OK;
}

int ft_recv_again(void *, void *, uint8_t *, int) { return DEMO_ERR_AGAIN; }
int ft_recv_again5(void *, void *, uint8_t *, int, net_addr_t *) { return DEMO_ERR_AGAIN; }
int ft_accept_again(void *, void *, void **, net_addr_t *) { return DEMO_ERR_AGAIN; }
int ft_send(void *, void *, const uint8_t *, int len) { return len; }
uint64_t ft_time(void *) { return 1000; }
uint32_t ft_random(void *) { return 42; }

const net_backend_t fake_backend_table = {
    nullptr, nullptr,                   /* init / deinit */
    ft_err_vp, ft_err_vssr, ft_err_v,   /* wifi_scan / sta_connect / sta_disconnect */
    ft_err_vss, ft_err_v, ft_err_vi,    /* ap_start / ap_stop / rssi */
    ft_err_v2, ft_err_vcb, ft_err_v2,   /* wifi_get_ip / current_ssid / gateway */
    ft_tcp_listen, ft_accept_again, ft_err_vav, /* tcp_listen / accept / connect */
    ft_send, ft_recv_again, ft_close,   /* send / recv / close */
    ft_mcast_join, ft_udp_send, ft_recv_again5, /* mcast_join / udp_send / udp_recv */
    ft_mdns_register, ft_mdns_unregister, ft_err_vcn, /* mdns */
    ft_err_vcv, ft_err_vcvn, ft_err_vc, /* nvs_get / nvs_set / nvs_erase */
    ft_time, ft_random, ft_err_vccc     /* time / random / inject */
};

net_ctx_t *MakeCtx(FakeBackend *fb)
{
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(&fake_backend_table, fb, nullptr, &ctx), DEMO_OK);
    return ctx;
}

} // namespace

TEST(HostLifecycle, StartOrderTcpBeforeMcastBeforeMdns)
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
        EXPECT_EQ(fb.calls[0], "tcp_listen");
        EXPECT_EQ(fb.calls[1], "mcast_join");
        EXPECT_EQ(fb.calls[2], "mdns_register");
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, TcpFailureSkipsAnnounceAndMdns)
{
    FakeBackend fb;
    fb.tcp_fail = true;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        EXPECT_NE(host.Start(), DEMO_OK);
        EXPECT_FALSE(host.Started());
        ASSERT_EQ(fb.calls.size(), (size_t)1);
        EXPECT_EQ(fb.calls[0], "tcp_listen");
        EXPECT_FALSE(fb.HasCall("mcast_join"));
        EXPECT_FALSE(fb.HasCall("mdns_register"));
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, McastFailureRollsBackListener)
{
    FakeBackend fb;
    fb.mcast_fail = true;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        EXPECT_NE(host.Start(), DEMO_OK);
        EXPECT_FALSE(host.Started());
        ASSERT_EQ(fb.calls.size(), (size_t)3);
        EXPECT_EQ(fb.calls[0], "tcp_listen");
        EXPECT_EQ(fb.calls[1], "mcast_join");
        EXPECT_EQ(fb.calls[2], "close"); /* listener 立即关闭 */
        EXPECT_TRUE(fb.WasClosed(fb.listen_handle));
        EXPECT_FALSE(fb.HasCall("mdns_register"));
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, MdnsFailureDoesNotBlockMulticastPath)
{
    FakeBackend fb;
    fb.mdns_fail = true;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        EXPECT_EQ(host.Start(), DEMO_OK); /* 组播主路径不受 mDNS 兼容注册失败影响 */
        EXPECT_TRUE(host.Started());
        EXPECT_EQ(fb.mdns_calls, 1);
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
        ASSERT_EQ(fb.calls.size(), (size_t)4);
        EXPECT_EQ(fb.calls[0], "udp_send");     /* SendBye */
        EXPECT_EQ(fb.calls[1], "close");        /* announcer.Stop */
        EXPECT_EQ(fb.calls[2], "close");        /* tcp_server.Stop */
        EXPECT_EQ(fb.calls[3], "mdns_unregister");
        EXPECT_TRUE(fb.WasClosed(fb.mcast_handle));
        EXPECT_TRUE(fb.WasClosed(fb.listen_handle));
        size_t close_count = (size_t)fb.close_calls;
        host.RequestStop(); /* 幂等：stop_ 已置位，无副作用 */
        EXPECT_EQ((size_t)fb.close_calls, close_count);
        EXPECT_FALSE(host.Started());
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, ForceCrashClosesAllWithoutBye)
{
    FakeBackend fb;
    demo_params_t params;
    params_defaults(&params);
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostApp host(params, ctx);
        ASSERT_EQ(host.Start(), DEMO_OK);
        fb.calls.clear();
        host.ForceCrash();
        EXPECT_FALSE(fb.HasCall("udp_send"));   /* 不发送 bye */
        EXPECT_TRUE(fb.WasClosed(fb.mcast_handle));
        EXPECT_TRUE(fb.WasClosed(fb.listen_handle));
        EXPECT_TRUE(fb.HasCall("mdns_unregister"));
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, TcpServerStartIdempotentStopIdempotent)
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
        EXPECT_EQ(server.Start(), DEMO_OK); /* 幂等成功，不再重复 bind */
        EXPECT_EQ(fb.listen_calls, 1);
        server.Stop();
        EXPECT_FALSE(server.Started());
        server.Stop(); /* 幂等 */
        EXPECT_EQ(fb.close_calls, 1);
        /* 停止后可再次启动 */
        EXPECT_EQ(server.Start(), DEMO_OK);
        EXPECT_TRUE(server.Started());
        server.Stop();
    }
    net_ctx_destroy(ctx);
}

TEST(HostLifecycle, AnnounceJsonUsesAdvertisedEndpoint)
{
    FakeBackend fb;
    demo_params_t params;
    params_defaults(&params);
    snprintf(params.host_virtual_ip, sizeof(params.host_virtual_ip), "192.168.1.50");
    params.host_tcp_port = 5935;
    net_ctx_t *ctx = MakeCtx(&fb);
    {
        HostAnnouncer announcer(ctx, params);
        ASSERT_EQ(announcer.Start(), DEMO_OK);
        announcer.Poll(1000);
        ASSERT_EQ(fb.udp_sends.size(), (size_t)1);
        cJSON *json = cJSON_Parse(fb.udp_sends[0].c_str());
        ASSERT_NE(json, nullptr);
        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(json, "ip");
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(json, "tcp_port");
        ASSERT_TRUE(cJSON_IsString(ip));
        ASSERT_TRUE(cJSON_IsNumber(port));
        EXPECT_EQ(std::string(ip->valuestring), std::string("192.168.1.50"));
        EXPECT_EQ(port->valueint, 5935);
        cJSON_Delete(json);
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
    EXPECT_NE(win_backend_ipv4_valid("0.0.0.0"), DEMO_OK);       /* unspecified */
    EXPECT_NE(win_backend_ipv4_valid("0.1.2.3"), DEMO_OK);       /* 0.0.0.0/8 */
    EXPECT_NE(win_backend_ipv4_valid("127.0.0.1"), DEMO_OK);     /* loopback */
    EXPECT_NE(win_backend_ipv4_valid("127.255.255.255"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("169.254.0.1"), DEMO_OK);   /* APIPA */
    EXPECT_NE(win_backend_ipv4_valid("169.254.255.255"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("192.168.1"), DEMO_OK);     /* 格式错误 */
    EXPECT_NE(win_backend_ipv4_valid("192.168.1.50.1"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("not-an-ip"), DEMO_OK);
    EXPECT_NE(win_backend_ipv4_valid("256.1.1.1"), DEMO_OK);     /* 越界 */
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
    /* 找一个空闲端口（顺序探测） */
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
    /* SO_EXCLUSIVEADDRUSE：同端口第二次监听必须失败 */
    void *second = nullptr;
    EXPECT_NE(net_tcp_listen(ctx, port, &second), DEMO_OK);
    EXPECT_EQ(second, nullptr);
    /* 关闭后可立即重新绑定（快速重启） */
    net_sock_close(ctx, first);
    void *third = nullptr;
    EXPECT_EQ(net_tcp_listen(ctx, port, &third), DEMO_OK);
    net_sock_close(ctx, third);
    net_ctx_destroy(ctx);
    win_backend_destroy(user);
}
#endif
