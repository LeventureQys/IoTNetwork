#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include "device_backend_factory.h"
#include "net_abstraction.h"
#include "protocol.h"
#include "common.h"
#include "sim_world.h"

namespace {

std::string MakeTempDir(const char *prefix)
{
    static int counter = 0;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                (std::string(prefix) + "_" + std::to_string(counter++));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

void CreateBackend(const char *nvs, const char *catalog, unsigned idx, unsigned port,
                   device_backend_instance_t *out)
{
    device_sim_backend_options_t o;
    memset(&o, 0, sizeof(o));
    o.config_path = "unused";
    o.nvs_file = nvs;
    o.sim_catalog_dir = catalog;
    o.target_ssid = "TactileFactory-2.4G";
    o.target_password = "modutech_leventure";
    o.host_virtual_ip = "192.168.1.50";
    o.device_index = idx;
    o.provision_port = port;
    o.random_seed = 12345u + idx * 7919u;
    device_error_t err;
    ASSERT_EQ(device_sim_backend_create(&o, out, &err), DEVICE_OK);
}

struct BackendPair {
    std::string root;
    std::string catalog;
    device_backend_instance_t a;
    device_backend_instance_t b;

    void SetUp()
    {
        root = MakeTempDir("simsock");
        catalog = root + "/catalog";
        std::string nvs_a = root + "/a.nvs.json";
        std::string nvs_b = root + "/b.nvs.json";
        CreateBackend(nvs_a.c_str(), catalog.c_str(), 0, 21000, &a);
        CreateBackend(nvs_b.c_str(), catalog.c_str(), 1, 21001, &b);
    }

    void TearDown()
    {
        a.destroy_user(a.user);
        b.destroy_user(b.user);
    }
};

int WaitForOk(const std::function<int()> &op, int max_tries = 100, int delay_ms = 10)
{
    int rc = DEMO_ERR_AGAIN;
    for (int i = 0; i < max_tries && rc == DEMO_ERR_AGAIN; i++) {
        rc = op();
        if (rc == DEMO_ERR_AGAIN)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    return rc;
}

} // namespace

TEST(SimSocket, LoopbackTcp)
{
    BackendPair bp;
    bp.SetUp();
    void *listen = nullptr;
    ASSERT_EQ(bp.a.vtable->tcp_listen(bp.a.user, 21100, &listen), DEMO_OK);
    void *conn = nullptr;
    net_addr_t addr;
    addr.ip = htonl(INADDR_LOOPBACK);
    addr.port = htons(21100);
    ASSERT_EQ(bp.b.vtable->tcp_connect(bp.b.user, &addr, &conn, 2000), DEMO_OK);
    void *accepted = nullptr;
    net_addr_t peer;
    int rc = WaitForOk([&]() { return bp.a.vtable->tcp_accept(bp.a.user, listen, &accepted, &peer); });
    ASSERT_EQ(rc, DEMO_OK);
    const char *msg = "hello";
    EXPECT_EQ(bp.b.vtable->sock_send(bp.b.user, conn, (const uint8_t *)msg, 5), 5);
    uint8_t buf[16];
    int n = WaitForOk([&]() { return bp.a.vtable->sock_recv(bp.a.user, accepted, buf, sizeof(buf)); });
    EXPECT_EQ(n, 5);
    EXPECT_EQ(memcmp(buf, msg, 5), 0);
    bp.b.vtable->sock_close(bp.b.user, conn);
    bp.a.vtable->sock_close(bp.a.user, accepted);
    bp.a.vtable->sock_close(bp.a.user, listen);
    bp.TearDown();
}

TEST(SimSocket, VirtualAddrTranslation)
{
    BackendPair bp;
    bp.SetUp();
    /* A 开 AP（模拟 PROTO_SIM_AP_IP:5935 ↔ 真实 21000，经 catalog 发布） */
    ASSERT_EQ(bp.a.vtable->wifi_ap_start(bp.a.user, "Modu_0001", "pass123456", "1234"), DEMO_OK);
    void *listen = nullptr;
    ASSERT_EQ(bp.a.vtable->tcp_listen(bp.a.user, 21000, &listen), DEMO_OK);
    /* B 连接模拟 AP 地址 → 应翻译到 127.0.0.1:21000 */
    net_addr_t vaddr;
    vaddr.ip = inet_addr(PROTO_SIM_AP_IP);
    vaddr.port = htons(PROTO_TCP_PORT);
    void *conn = nullptr;
    ASSERT_EQ(bp.b.vtable->tcp_connect(bp.b.user, &vaddr, &conn, 2000), DEMO_OK);
    void *accepted = nullptr;
    int rc = WaitForOk([&]() { return bp.a.vtable->tcp_accept(bp.a.user, listen, &accepted, nullptr); });
    ASSERT_EQ(rc, DEMO_OK);
    bp.b.vtable->sock_close(bp.b.user, conn);
    bp.a.vtable->sock_close(bp.a.user, accepted);
    bp.a.vtable->sock_close(bp.a.user, listen);
    ASSERT_EQ(bp.a.vtable->wifi_ap_stop(bp.a.user), DEMO_OK);
    bp.TearDown();
}

TEST(SimSocket, HostVirtualAddrConnect)
{
    BackendPair bp;
    bp.SetUp();
    /* host 业务端口监听（host 在虚拟 IP 192.168.1.50）→ 翻译到 loopback:5935 */
    void *listen = nullptr;
    ASSERT_EQ(bp.a.vtable->tcp_listen(bp.a.user, PROTO_TCP_PORT, &listen), DEMO_OK);
    net_addr_t vaddr;
    vaddr.ip = sim_world_host_virtual_ip();
    vaddr.port = htons(PROTO_TCP_PORT);
    void *conn = nullptr;
    ASSERT_EQ(bp.b.vtable->tcp_connect(bp.b.user, &vaddr, &conn, 2000), DEMO_OK);
    void *accepted = nullptr;
    int rc = WaitForOk([&]() { return bp.a.vtable->tcp_accept(bp.a.user, listen, &accepted, nullptr); });
    ASSERT_EQ(rc, DEMO_OK);
    bp.b.vtable->sock_close(bp.b.user, conn);
    bp.a.vtable->sock_close(bp.a.user, accepted);
    bp.a.vtable->sock_close(bp.a.user, listen);
    bp.TearDown();
}

TEST(SimSocket, MulticastBetweenInstances)
{
    BackendPair bp;
    bp.SetUp();
    void *hs = nullptr, *ds = nullptr;
    ASSERT_EQ(bp.a.vtable->udp_mcast_join(bp.a.user, PROTO_MCAST_GROUP, PROTO_MCAST_PORT, &hs), DEMO_OK);
    ASSERT_EQ(bp.b.vtable->udp_mcast_join(bp.b.user, PROTO_MCAST_GROUP, PROTO_MCAST_PORT, &ds), DEMO_OK);
    const char *announce = "{\"cmd\":\"host_announce\"}";
    ASSERT_EQ(bp.a.vtable->udp_send(bp.a.user, PROTO_MCAST_GROUP, PROTO_MCAST_PORT,
                                    (const uint8_t *)announce, (int)strlen(announce)),
              (int)strlen(announce));
    uint8_t buf[256];
    int n = WaitForOk([&]() { return bp.b.vtable->udp_recv(bp.b.user, ds, buf, sizeof(buf), nullptr); },
                      200);
    EXPECT_EQ(n, (int)strlen(announce));
    EXPECT_EQ(memcmp(buf, announce, (size_t)n), 0);
    bp.a.vtable->sock_close(bp.a.user, hs);
    bp.b.vtable->sock_close(bp.b.user, ds);
    bp.TearDown();
}

TEST(SimSocket, NonBlockingRecv)
{
    BackendPair bp;
    bp.SetUp();
    void *hs = nullptr, *ds = nullptr;
    ASSERT_EQ(bp.a.vtable->udp_mcast_join(bp.a.user, PROTO_MCAST_GROUP, PROTO_MCAST_PORT, &hs), DEMO_OK);
    ASSERT_EQ(bp.b.vtable->udp_mcast_join(bp.b.user, PROTO_MCAST_GROUP, PROTO_MCAST_PORT, &ds), DEMO_OK);
    uint8_t buf[64];
    EXPECT_EQ(bp.b.vtable->udp_recv(bp.b.user, ds, buf, sizeof(buf), nullptr), DEMO_ERR_AGAIN);
    bp.a.vtable->sock_close(bp.a.user, hs);
    bp.b.vtable->sock_close(bp.b.user, ds);
    bp.TearDown();
}

TEST(SimSocket, ConnectTimeout)
{
    BackendPair bp;
    bp.SetUp();
    net_addr_t addr;
    addr.ip = htonl(INADDR_LOOPBACK);
    addr.port = htons(59999); /* 未监听端口 */
    void *conn = nullptr;
    int rc = bp.a.vtable->tcp_connect(bp.a.user, &addr, &conn, 500);
    EXPECT_EQ(rc, DEMO_ERR_TIMEOUT);
    bp.TearDown();
}
