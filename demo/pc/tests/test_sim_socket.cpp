#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include "net_abstraction.h"
#include "protocol.h"
#include "sim_backend.h"
#include "sim_world.h"
#include "params.h"
#include "log.h"

namespace {

struct BackendPair {
    demo_params_t params;
    net_ctx_t *host_ctx = nullptr;
    net_ctx_t *dev_ctx = nullptr;
    void *host_user = nullptr;
    void *dev_user = nullptr;

    void Init()
    {
        params_defaults(&params);
        params.host_tcp_port = 5935;
        host_user = sim_backend_create("host", &params);
        dev_user = sim_backend_create("dev0", &params);
        net_ctx_t *hc = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), host_user, nullptr, &hc), DEMO_OK);
        host_ctx = hc;
        net_ctx_t *dc = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), dev_user, nullptr, &dc), DEMO_OK);
        dev_ctx = dc;
    }

    void Cleanup()
    {
        if (host_ctx) net_ctx_destroy(host_ctx);
        if (dev_ctx) net_ctx_destroy(dev_ctx);
        if (host_user) sim_backend_destroy(host_user);
        if (dev_user) sim_backend_destroy(dev_user);
        host_ctx = dev_ctx = nullptr;
        host_user = dev_user = nullptr;
    }
};

} // namespace

TEST(SimSocket, LoopbackTcp)
{
    BackendPair bp;
    bp.Init();
    void *listen = nullptr;
    ASSERT_EQ(net_tcp_listen(bp.dev_ctx, 21100, &listen), DEMO_OK);
    void *conn = nullptr;
    net_addr_t addr;
    addr.ip = htonl(INADDR_LOOPBACK);
    addr.port = htons(21100);
    ASSERT_EQ(net_tcp_connect(bp.host_ctx, &addr, &conn, 2000), DEMO_OK);
    void *accepted = nullptr;
    net_addr_t peer;
    int rc = DEMO_ERR_AGAIN;
    for (int i = 0; i < 100 && rc == DEMO_ERR_AGAIN; i++) {
        rc = net_tcp_accept(bp.dev_ctx, listen, &accepted, &peer);
        if (rc == DEMO_ERR_AGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(rc, DEMO_OK);
    const char *msg = "hello";
    EXPECT_EQ(net_sock_send(bp.host_ctx, conn, (const uint8_t *)msg, 5), 5);
    uint8_t buf[16];
    int n = 0;
    for (int i = 0; i < 100; i++) {
        n = net_sock_recv(bp.dev_ctx, accepted, buf, sizeof(buf));
        if (n != DEMO_ERR_AGAIN) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(n, 5);
    EXPECT_EQ(memcmp(buf, msg, 5), 0);
    net_sock_close(bp.host_ctx, conn);
    net_sock_close(bp.dev_ctx, accepted);
    net_sock_close(bp.dev_ctx, listen);
    bp.Cleanup();
}

TEST(SimSocket, MulticastBetweenInstances)
{
    BackendPair bp;
    bp.Init();
    void *hs = nullptr, *ds = nullptr;
    ASSERT_EQ(net_udp_mcast_join(bp.host_ctx, "224.0.2.1", 5936, &hs), DEMO_OK);
    ASSERT_EQ(net_udp_mcast_join(bp.dev_ctx, "224.0.2.1", 5936, &ds), DEMO_OK);
    const char *announce = "{\"cmd\":\"ping\"}";
    ASSERT_EQ(net_udp_send(bp.host_ctx, "224.0.2.1", 5936, (const uint8_t *)announce,
                           (int)strlen(announce)),
              (int)strlen(announce));
    uint8_t buf[256];
    int n = 0;
    for (int i = 0; i < 200; i++) {
        n = net_udp_recv(bp.dev_ctx, ds, buf, sizeof(buf), nullptr);
        if (n != DEMO_ERR_AGAIN) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(n, (int)strlen(announce));
    EXPECT_EQ(memcmp(buf, announce, (size_t)n), 0);
    net_sock_close(bp.host_ctx, hs);
    net_sock_close(bp.dev_ctx, ds);
    bp.Cleanup();
}

TEST(SimSocket, ConnectTimeout)
{
    BackendPair bp;
    bp.Init();
    net_addr_t addr;
    addr.ip = htonl(INADDR_LOOPBACK);
    addr.port = htons(59999); /* 未监听端口 */
    void *conn = nullptr;
    int rc = net_tcp_connect(bp.host_ctx, &addr, &conn, 500);
    EXPECT_EQ(rc, DEMO_ERR_TIMEOUT);
    bp.Cleanup();
}
