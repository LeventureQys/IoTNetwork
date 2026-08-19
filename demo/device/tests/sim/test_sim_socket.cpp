#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>
#include <string>
#include <functional>
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
#include "sim_ap_catalog.h"

namespace {

/* mDNS/组播为 v1.1 之前的服务发现通道，vtable 仍保留；协议常量已裁剪，
 * 此处使用与旧值一致的本地字面量。 */
static const char *kMcastGroup = "224.0.2.1";
static const uint16_t kMcastPort = 5936;

std::string MakeTempDir(const char *prefix)
{
    static int counter = 0;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                (std::string(prefix) + "_" + std::to_string(counter++));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

void CreateBackend(const char *nvs, const char *catalog, unsigned idx,
                   device_backend_instance_t *out)
{
    device_sim_backend_options_t o;
    memset(&o, 0, sizeof(o));
    o.config_path = "unused";
    o.nvs_file = nvs;
    o.sim_catalog_dir = catalog;
    o.device_index = idx;
    o.random_seed = 12345u + idx * 7919u;
    device_error_t err;
    ASSERT_EQ(device_sim_backend_create(&o, out, &err), DEVICE_OK);
}

void WriteHotspot(const std::string &catalog, const char *ssid, const char *password,
                  unsigned loopback_port)
{
    std::filesystem::create_directories(catalog);
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"schema\":2,\"ssid\":\"%s\",\"password\":\"%s\","
             "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
             "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
             "\"loopback_port\":%u,\"published_at_ms\":%llu,\"owner_pid\":%lu}",
             ssid, password, loopback_port,
             (unsigned long long)sim_ap_catalog_wallclock_ms(),
             sim_ap_catalog_current_pid());
    FILE *fp = fopen((catalog + "/pc-hotspot.json").c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fputs(buf, fp);
    fclose(fp);
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
        CreateBackend(nvs_a.c_str(), catalog.c_str(), 0, &a);
        CreateBackend(nvs_b.c_str(), catalog.c_str(), 1, &b);
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

TEST(SimSocket, PcApAddrTranslation)
{
    BackendPair bp;
    bp.SetUp();
    /* PC 发布：loopback_port=21000，a 在 21000 监听 */
    WriteHotspot(bp.catalog, "Modu_PC", "modu_leventure", 21000);
    void *listen = nullptr;
    ASSERT_EQ(bp.a.vtable->tcp_listen(bp.a.user, 21000, &listen), DEMO_OK);
    /* b 连接 192.168.137.1:5935 → 翻译到 127.0.0.1:21000 */
    net_addr_t vaddr;
    vaddr.ip = inet_addr(PROTO_PC_AP_IP);
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
    ASSERT_EQ(bp.a.vtable->udp_mcast_join(bp.a.user, kMcastGroup, kMcastPort, &hs), DEMO_OK);
    ASSERT_EQ(bp.b.vtable->udp_mcast_join(bp.b.user, kMcastGroup, kMcastPort, &ds), DEMO_OK);
    const char *announce = "{\"cmd\":\"host_announce\"}";
    ASSERT_EQ(bp.a.vtable->udp_send(bp.a.user, kMcastGroup, kMcastPort,
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
    ASSERT_EQ(bp.a.vtable->udp_mcast_join(bp.a.user, kMcastGroup, kMcastPort, &hs), DEMO_OK);
    ASSERT_EQ(bp.b.vtable->udp_mcast_join(bp.b.user, kMcastGroup, kMcastPort, &ds), DEMO_OK);
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
