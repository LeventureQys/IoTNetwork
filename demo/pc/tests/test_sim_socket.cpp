#include <gtest/gtest.h>
#include <cstring>
#include <thread>
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

    void Init(int dev_index = 0)
    {
        params_defaults(&params);
        params.device_ap_port_base = 21000 + dev_index * 10;
        params.host_tcp_port = 5935;
        host_user = sim_backend_create("host", &params);
        dev_user = sim_backend_create("dev0", &params);
        net_ctx_t *host_ctx_tmp = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), host_user, nullptr, &host_ctx_tmp), DEMO_OK);
        host_ctx = host_ctx_tmp;
        net_ctx_t *dev_ctx_tmp = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), dev_user, nullptr, &dev_ctx_tmp), DEMO_OK);
        dev_ctx = dev_ctx_tmp;
        SimWorld::Instance().TargetNetworkSet(params.target_ssid, params.target_password, true);
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

TEST(SimSocket, VirtualAddrTranslation)
{
    BackendPair bp;
    bp.Init();
    /* device 寮€ AP锛堟ā鎷?PROTO_SIM_AP_IP:5935 鈫?鐪熷疄 21000锛?*/
    ASSERT_EQ(net_wifi_ap_start(bp.dev_ctx, "Modu_0001", "pass123456", "1234"), DEMO_OK);
    /* device 鐩戝惉 AP 鐪熷疄绔彛 */
    void *listen = nullptr;
    ASSERT_EQ(net_tcp_listen(bp.dev_ctx, 21000, &listen), DEMO_OK);
    /* host 杩炴帴妯℃嫙 AP 鍦板潃 鈫?搴旂炕璇戝埌鐪熷疄 127.0.0.1:21000 */
    net_addr_t vaddr;
    vaddr.ip = inet_addr(PROTO_SIM_AP_IP);
    vaddr.port = htons(PROTO_TCP_PORT);
    void *conn = nullptr;
    ASSERT_EQ(net_tcp_connect(bp.host_ctx, &vaddr, &conn, 2000), DEMO_OK);
    void *accepted = nullptr;
    net_addr_t peer;
    int rc = DEMO_ERR_AGAIN;
    for (int i = 0; i < 100 && rc == DEMO_ERR_AGAIN; i++) {
        rc = net_tcp_accept(bp.dev_ctx, listen, &accepted, &peer);
        if (rc == DEMO_ERR_AGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(rc, DEMO_OK);
    net_sock_close(bp.host_ctx, conn);
    net_sock_close(bp.dev_ctx, accepted);
    net_sock_close(bp.dev_ctx, listen);
    net_wifi_ap_stop(bp.dev_ctx);
    bp.Cleanup();
}

TEST(SimSocket, HostVirtualAddrConnect)
{
    BackendPair bp;
    bp.Init();
    /* host 涓氬姟绔彛鐩戝惉锛坔ost 鍦ㄨ櫄鎷?IP 192.168.1.50锛?*/
    void *listen = nullptr;
    ASSERT_EQ(net_tcp_listen(bp.host_ctx, (uint16_t)bp.params.host_tcp_port, &listen), DEMO_OK);
    /* device 杩炴帴铏氭嫙 host 鍦板潃 鈫?缈昏瘧鍒?127.0.0.1:5935 */
    net_addr_t vaddr;
    vaddr.ip = SimWorld::HostVirtualIp();
    vaddr.port = htons(5935);
    void *conn = nullptr;
    ASSERT_EQ(net_tcp_connect(bp.dev_ctx, &vaddr, &conn, 2000), DEMO_OK);
    void *accepted = nullptr;
    int rc = DEMO_ERR_AGAIN;
    for (int i = 0; i < 100 && rc == DEMO_ERR_AGAIN; i++) {
        rc = net_tcp_accept(bp.host_ctx, listen, &accepted, nullptr);
        if (rc == DEMO_ERR_AGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(rc, DEMO_OK);
    net_sock_close(bp.dev_ctx, conn);
    net_sock_close(bp.host_ctx, accepted);
    net_sock_close(bp.host_ctx, listen);
    bp.Cleanup();
}

TEST(SimSocket, MulticastBetweenInstances)
{
    BackendPair bp;
    bp.Init();
    void *hs = nullptr, *ds = nullptr;
    ASSERT_EQ(net_udp_mcast_join(bp.host_ctx, "224.0.2.1", 5936, &hs), DEMO_OK);
    ASSERT_EQ(net_udp_mcast_join(bp.dev_ctx, "224.0.2.1", 5936, &ds), DEMO_OK);
    const char *announce = "{\"cmd\":\"host_announce\"}";
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

TEST(SimSocket, NonBlockingRecv)
{
    BackendPair bp;
    bp.Init();
    void *hs = nullptr, *ds = nullptr;
    ASSERT_EQ(net_udp_mcast_join(bp.host_ctx, "224.0.2.1", 5936, &hs), DEMO_OK);
    ASSERT_EQ(net_udp_mcast_join(bp.dev_ctx, "224.0.2.1", 5936, &ds), DEMO_OK);
    uint8_t buf[64];
    EXPECT_EQ(net_udp_recv(bp.dev_ctx, ds, buf, sizeof(buf), nullptr), DEMO_ERR_AGAIN);
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
    addr.port = htons(59999); /* 鏈洃鍚鍙?*/
    void *conn = nullptr;
    int rc = net_tcp_connect(bp.host_ctx, &addr, &conn, 500);
    EXPECT_EQ(rc, DEMO_ERR_TIMEOUT);
    bp.Cleanup();
}
