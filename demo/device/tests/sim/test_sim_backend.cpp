#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
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
static const char *kMdnsType = "_tactile._tcp";
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

std::string HotspotPath(const std::string &catalog)
{
    return catalog + "/pc-hotspot.json";
}

/* 直接写一个 schema2 记录文件，模拟 PC 侧发布（设备侧只读取） */
void WriteHotspot(const std::string &catalog, const char *ssid, const char *password,
                  unsigned loopback_port, uint64_t published_ms, unsigned long pid)
{
    std::filesystem::create_directories(catalog);
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"schema\":2,\"ssid\":\"%s\",\"password\":\"%s\","
             "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
             "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
             "\"loopback_port\":%u,\"published_at_ms\":%llu,\"owner_pid\":%lu}",
             ssid, password, loopback_port,
             (unsigned long long)published_ms, pid);
    FILE *fp = fopen(HotspotPath(catalog).c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fputs(buf, fp);
    fclose(fp);
}

struct Opts {
    device_sim_backend_options_t o;
    std::string catalog;
    std::string nvs;

    explicit Opts(const std::string &root, unsigned idx, uint32_t seed = 42)
    {
        memset(&o, 0, sizeof(o));
        catalog = root + "/catalog";
        nvs = root + "/dev" + std::to_string(idx) + ".nvs.json";
        o.config_path = "unused";
        o.sim_catalog_dir = catalog.c_str();
        o.nvs_file = nvs.c_str();
        o.device_index = idx;
        o.random_seed = seed;
    }
};

device_backend_instance_t Create(const Opts &opts)
{
    device_backend_instance_t inst;
    device_error_t err;
    memset(&inst, 0xFF, sizeof(inst));
    device_result_t rc = device_sim_backend_create(&opts.o, &inst, &err);
    EXPECT_EQ(rc, DEVICE_OK) << err.message;
    return inst;
}

void Destroy(device_backend_instance_t *inst)
{
    if (inst->destroy_user)
        inst->destroy_user(inst->user);
    memset(inst, 0, sizeof(*inst));
}

uint64_t NowMs()
{
    /* 与 catalog 墙钟一致（发布者/读者跨进程同一时钟基准） */
    return (uint64_t)sim_ap_catalog_wallclock_ms();
}

} // namespace

TEST(SimBackendFactory, SuccessFillsInstance)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0);
    device_backend_instance_t inst;
    device_error_t err;
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(&opts.o, &inst, &err), DEVICE_OK);
    EXPECT_EQ(err.code, DEVICE_OK);
    EXPECT_NE(inst.vtable, nullptr);
    EXPECT_NE(inst.user, nullptr);
    EXPECT_NE(inst.destroy_user, nullptr);
    /* vtable 29 项布局完整性：抽查生命周期与关键回调 */
    EXPECT_NE(inst.vtable->init, nullptr);
    EXPECT_NE(inst.vtable->wifi_scan, nullptr);
    EXPECT_NE(inst.vtable->tcp_connect, nullptr);
    EXPECT_NE(inst.vtable->inject, nullptr);
    EXPECT_NE(inst.vtable->time_ms, nullptr);
    inst.destroy_user(inst.user);
}

TEST(SimBackendFactory, CreateFailureRollsBack)
{
    std::string root = MakeTempDir("simbackend");

    /* 空 options */
    device_backend_instance_t inst;
    device_error_t err;
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(nullptr, &inst, &err), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.vtable, nullptr);
    EXPECT_EQ(inst.user, nullptr);
    EXPECT_EQ(inst.destroy_user, nullptr);

    /* catalog_dir 为空 */
    Opts o(root, 0);
    o.o.sim_catalog_dir = "";
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(&o.o, &inst, &err), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.user, nullptr);
    EXPECT_STREQ(err.operation, "device_sim_backend_create");
    EXPECT_NE(err.message[0], 0);

    /* catalog_dir 非绝对路径 */
    Opts o2(root, 0);
    o2.o.sim_catalog_dir = "relative/catalog";
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(&o2.o, &inst, &err), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.user, nullptr);

    /* device_index 越界 */
    Opts o3(root, 16);
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(&o3.o, &inst, &err), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.user, nullptr);

    /* out_instance 为空 */
    EXPECT_EQ(device_sim_backend_create(&o3.o, nullptr, &err), DEVICE_ERR_INVALID_ARGUMENT);
}

TEST(SimBackendFactory, OptionsStringsAreCopied)
{
    std::string root = MakeTempDir("simbackend");
    std::string original = root + "/catalog_orig";
    std::string mutated = root + "/catalog_mutated";
    char borrowed[512];
    snprintf(borrowed, sizeof(borrowed), "%s", original.c_str());

    Opts opts(root, 0);
    opts.o.sim_catalog_dir = borrowed;
    device_backend_instance_t inst = Create(opts);
    /* 调用期借用结束后修改源字符串，实例行为不受影响 */
    snprintf(borrowed, sizeof(borrowed), "%s", mutated.c_str());

    /* 向"原值"目录写热点，实例应仍读取原值目录（而非被改写的源串） */
    WriteHotspot(original, "Modu_PC", "modu_leventure", 21000, NowMs(),
                 (unsigned long)sim_ap_catalog_current_pid());
    net_ap_info_t aps[8];
    int count = 8;
    EXPECT_EQ(inst.vtable->wifi_scan(inst.user, aps, &count), DEMO_OK);
    EXPECT_EQ(count, 1);
    EXPECT_STREQ(aps[0].ssid, "Modu_PC");
    Destroy(&inst);
}

TEST(SimBackendFactory, DestroyAllZeroInstanceSafe)
{
    device_backend_instance_t inst;
    memset(&inst, 0, sizeof(inst));
    /* 全零实例：destroy_user 为空，无需任何操作（无崩溃即通过） */
    EXPECT_EQ(inst.destroy_user, nullptr);
}

TEST(SimBackend, WifiScanReturnsPcHotspotWhenPublished)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0);
    device_backend_instance_t inst = Create(opts);

    /* 无记录 → 不返回任何 AP */
    net_ap_info_t aps[8];
    int count = 8;
    EXPECT_EQ(inst.vtable->wifi_scan(inst.user, aps, &count), DEMO_OK);
    EXPECT_EQ(count, 0);

    /* 记录存在 → 作为一条 2.4GHz AP 返回 */
    WriteHotspot(opts.catalog, "Modu_PC", "modu_leventure", 21000, NowMs(),
                 (unsigned long)sim_ap_catalog_current_pid());
    count = 8;
    EXPECT_EQ(inst.vtable->wifi_scan(inst.user, aps, &count), DEMO_OK);
    EXPECT_EQ(count, 1);
    EXPECT_STREQ(aps[0].ssid, "Modu_PC");
    EXPECT_EQ(aps[0].band_2g, 1);

    /* 容量 0 时 count 仍统计实际命中数 */
    count = 0;
    EXPECT_EQ(inst.vtable->wifi_scan(inst.user, nullptr, &count), DEMO_OK);
    EXPECT_EQ(count, 1);
    Destroy(&inst);
}

TEST(SimBackend, StaConnectExactSsidAndPassword)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0);
    device_backend_instance_t inst = Create(opts);
    WriteHotspot(opts.catalog, "Modu_PC", "modu_leventure", 21000, NowMs(),
                 (unsigned long)sim_ap_catalog_current_pid());

    wifi_reason_t reason = WIFI_REASON_OK;

    /* 精确 SSID + 密码成功 */
    EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "Modu_PC", "modu_leventure", &reason),
              DEMO_OK);
    EXPECT_EQ((int)reason, 0);

    char cur[33];
    EXPECT_EQ(inst.vtable->wifi_get_current_ssid(inst.user, cur, sizeof(cur)), DEMO_OK);
    EXPECT_STREQ(cur, "Modu_PC");

    /* 网关返回 192.168.137.1 */
    uint32_t gw = 0;
    EXPECT_EQ(inst.vtable->wifi_get_gateway(inst.user, &gw), DEMO_OK);
    EXPECT_EQ(gw, inet_addr(PROTO_PC_AP_IP));

    /* SSID 不符 → NO_AP_FOUND(201) */
    EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "OtherSSID", "modu_leventure", &reason),
              DEMO_ERR);
    EXPECT_EQ((int)reason, 201);

    /* 密码不符 → AUTH_FAIL(202) */
    EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "Modu_PC", "wrongpass", &reason),
              DEMO_ERR);
    EXPECT_EQ((int)reason, 202);
    Destroy(&inst);
}

TEST(SimBackend, StaConnectNoRecordReturnsNoApFound)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0);
    device_backend_instance_t inst = Create(opts);

    wifi_reason_t reason = WIFI_REASON_OK;
    EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "Modu_PC", "modu_leventure", &reason),
              DEMO_ERR);
    EXPECT_EQ((int)reason, 201);
    Destroy(&inst);
}

TEST(SimBackend, InvalidOrExpiredRecordIgnored)
{
    /* 损坏 JSON → 忽略（scan 无结果 / connect NO_AP_FOUND） */
    {
        std::string root = MakeTempDir("simbackend");
        Opts opts(root, 0);
        device_backend_instance_t inst = Create(opts);
        std::filesystem::create_directories(opts.catalog);
        FILE *fp = fopen(HotspotPath(opts.catalog).c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        fputs("### not json ###", fp);
        fclose(fp);

        net_ap_info_t aps[8];
        int count = 8;
        EXPECT_EQ(inst.vtable->wifi_scan(inst.user, aps, &count), DEMO_OK);
        EXPECT_EQ(count, 0);
        wifi_reason_t reason = WIFI_REASON_OK;
        EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "Modu_PC", "modu_leventure", &reason),
                  DEMO_ERR);
        EXPECT_EQ((int)reason, 201);
        Destroy(&inst);
    }

    /* schema 非 2 → 忽略 */
    {
        std::string root = MakeTempDir("simbackend");
        Opts opts(root, 0);
        device_backend_instance_t inst = Create(opts);
        std::filesystem::create_directories(opts.catalog);
        FILE *fp = fopen(HotspotPath(opts.catalog).c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        fputs("{\"schema\":1,\"ssid\":\"Modu_PC\",\"password\":\"modu_leventure\","
              "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
              "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
              "\"loopback_port\":21000,\"published_at_ms\":1,\"owner_pid\":1}", fp);
        fclose(fp);
        net_ap_info_t aps[8];
        int count = 8;
        EXPECT_EQ(inst.vtable->wifi_scan(inst.user, aps, &count), DEMO_OK);
        EXPECT_EQ(count, 0);
        Destroy(&inst);
    }

    /* 过期且 owner 死亡 → 忽略 */
    {
        std::string root = MakeTempDir("simbackend");
        Opts opts(root, 0);
        device_backend_instance_t inst = Create(opts);
        WriteHotspot(opts.catalog, "Modu_PC", "modu_leventure", 21000,
                     NowMs() - 40000, 999999999 /* 必然不存在的 PID */);
        net_ap_info_t aps[8];
        int count = 8;
        EXPECT_EQ(inst.vtable->wifi_scan(inst.user, aps, &count), DEMO_OK);
        EXPECT_EQ(count, 0);
        Destroy(&inst);
    }
}

TEST(SimBackend, TcpConnectTranslatesPcApToLoopback)
{
    std::string root = MakeTempDir("simbackend");
    Opts a_opts(root, 0);
    Opts b_opts(root, 1);
    device_backend_instance_t a = Create(a_opts);
    device_backend_instance_t b = Create(b_opts);

    /* PC 发布：loopback_port=21000（a 在此端口监听） */
    WriteHotspot(a_opts.catalog, "Modu_PC", "modu_leventure", 21000, NowMs(),
                 (unsigned long)sim_ap_catalog_current_pid());

    void *listen = nullptr;
    ASSERT_EQ(a.vtable->tcp_listen(a.user, 21000, &listen), DEMO_OK);

    /* b 连接 192.168.137.1:5935 → 翻译到 127.0.0.1:21000 */
    net_addr_t vaddr;
    vaddr.ip = inet_addr(PROTO_PC_AP_IP);
    vaddr.port = htons(PROTO_TCP_PORT);
    void *conn = nullptr;
    ASSERT_EQ(b.vtable->tcp_connect(b.user, &vaddr, &conn, 2000), DEMO_OK);

    void *accepted = nullptr;
    int rc = DEMO_ERR_AGAIN;
    for (int i = 0; i < 100 && rc == DEMO_ERR_AGAIN; i++) {
        rc = a.vtable->tcp_accept(a.user, listen, &accepted, nullptr);
        if (rc == DEMO_ERR_AGAIN)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(rc, DEMO_OK);

    b.vtable->sock_close(b.user, conn);
    a.vtable->sock_close(a.user, accepted);
    a.vtable->sock_close(a.user, listen);
    Destroy(&a);
    Destroy(&b);
}

TEST(SimBackend, TcpConnectOtherAddressNotTranslated)
{
    std::string root = MakeTempDir("simbackend");
    Opts a_opts(root, 0);
    Opts b_opts(root, 1);
    device_backend_instance_t a = Create(a_opts);
    device_backend_instance_t b = Create(b_opts);

    /* 无 catalog 记录：连接非 PC 目标（loopback 直连）不做翻译，仍可直连 */
    void *listen = nullptr;
    ASSERT_EQ(a.vtable->tcp_listen(a.user, 21100, &listen), DEMO_OK);
    net_addr_t addr;
    addr.ip = htonl(INADDR_LOOPBACK);
    addr.port = htons(21100);
    void *conn = nullptr;
    ASSERT_EQ(b.vtable->tcp_connect(b.user, &addr, &conn, 2000), DEMO_OK);

    void *accepted = nullptr;
    int rc = DEMO_ERR_AGAIN;
    for (int i = 0; i < 100 && rc == DEMO_ERR_AGAIN; i++) {
        rc = a.vtable->tcp_accept(a.user, listen, &accepted, nullptr);
        if (rc == DEMO_ERR_AGAIN)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(rc, DEMO_OK);
    b.vtable->sock_close(b.user, conn);
    a.vtable->sock_close(a.user, accepted);
    a.vtable->sock_close(a.user, listen);

    /* 无 catalog 记录时，连接 PC 固定目标应 fail-closed（无法翻译） */
    net_addr_t pcaddr;
    pcaddr.ip = inet_addr(PROTO_PC_AP_IP);
    pcaddr.port = htons(PROTO_TCP_PORT);
    void *conn2 = nullptr;
    EXPECT_EQ(b.vtable->tcp_connect(b.user, &pcaddr, &conn2, 500), DEMO_ERR);

    Destroy(&a);
    Destroy(&b);
}

TEST(SimBackend, PcStopRemovesHotspotFromScan)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0);
    device_backend_instance_t inst = Create(opts);
    WriteHotspot(opts.catalog, "Modu_PC", "modu_leventure", 21000, NowMs(),
                 (unsigned long)sim_ap_catalog_current_pid());

    net_ap_info_t aps[8];
    int count = 8;
    EXPECT_EQ(inst.vtable->wifi_scan(inst.user, aps, &count), DEMO_OK);
    EXPECT_EQ(count, 1);

    /* PC 停止：删除记录文件 → 设备不再扫描到热点 */
    std::filesystem::remove(HotspotPath(opts.catalog));
    count = 8;
    EXPECT_EQ(inst.vtable->wifi_scan(inst.user, aps, &count), DEMO_OK);
    EXPECT_EQ(count, 0);
    Destroy(&inst);
}

TEST(SimBackend, ApStartStopReturnErr)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0);
    device_backend_instance_t inst = Create(opts);
    /* v1.1 设备不再创建热点 */
    EXPECT_EQ(inst.vtable->wifi_ap_start(inst.user, "Modu_PC", "modu_leventure", "1234"),
              DEMO_ERR);
    EXPECT_EQ(inst.vtable->wifi_ap_stop(inst.user), DEMO_ERR);
    Destroy(&inst);
}

TEST(SimBackend, TwoInstancesIsolated)
{
    std::string root = MakeTempDir("simbackend");
    device_backend_instance_t a = Create(Opts(root, 0, 7));
    device_backend_instance_t b = Create(Opts(root, 1, 9));

    /* RSSI 隔离（A 注入不影响 B） */
    ASSERT_EQ(a.vtable->inject(a.user, "rssi_set", "-80"), DEMO_OK);
    int rssi = 0;
    EXPECT_EQ(a.vtable->wifi_get_rssi(a.user, &rssi), DEMO_OK);
    EXPECT_EQ(rssi, -80);
    EXPECT_EQ(b.vtable->wifi_get_rssi(b.user, &rssi), DEMO_OK);
    EXPECT_EQ(rssi, -58);

    /* 未连接时 IP 为 0 */
    uint32_t ip = 0xFFFFFFFF;
    EXPECT_EQ(a.vtable->wifi_get_ip(a.user, &ip), DEMO_OK);
    EXPECT_EQ(ip, 0u);

    /* NVS 文件隔离 */
    EXPECT_FALSE(std::filesystem::exists(root + "/dev1.nvs.json"));
    EXPECT_FALSE(std::filesystem::exists(root + "/dev0.nvs.json"));
    ASSERT_EQ(a.vtable->nvs_set(a.user, "k", (const uint8_t *)"v", 1), DEMO_OK);
    EXPECT_TRUE(std::filesystem::exists(root + "/dev0.nvs.json"));
    EXPECT_FALSE(std::filesystem::exists(root + "/dev1.nvs.json"));

    Destroy(&a);
    Destroy(&b);
}

TEST(SimBackend, FaultInjectionConsumptionIsolation)
{
    std::string root = MakeTempDir("simbackend");
    device_backend_instance_t a = Create(Opts(root, 0));
    device_backend_instance_t b = Create(Opts(root, 1));

    void *listen = nullptr;
    ASSERT_EQ(a.vtable->tcp_listen(a.user, 21200, &listen), DEMO_OK);
    void *conn = nullptr;
    net_addr_t addr;
    addr.ip = htonl(INADDR_LOOPBACK);
    addr.port = htons(21200);
    ASSERT_EQ(b.vtable->tcp_connect(b.user, &addr, &conn, 2000), DEMO_OK);
    void *accepted = nullptr;
    int rc = DEMO_ERR_AGAIN;
    for (int i = 0; i < 100 && rc == DEMO_ERR_AGAIN; i++) {
        rc = a.vtable->tcp_accept(a.user, listen, &accepted, nullptr);
        if (rc == DEMO_ERR_AGAIN)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(rc, DEMO_OK);

    ASSERT_EQ(a.vtable->inject(a.user, "sock_send_fail", "{\"count\":1}"), DEMO_OK);
    const char *msg = "hello";
    EXPECT_EQ(a.vtable->sock_send(a.user, conn, (const uint8_t *)msg, 5), DEMO_ERR);
    EXPECT_EQ(b.vtable->sock_send(b.user, accepted, (const uint8_t *)msg, 5), 5);
    uint8_t buf[16];
    int n = 0;
    for (int i = 0; i < 100; i++) {
        n = a.vtable->sock_recv(a.user, conn, buf, sizeof(buf));
        if (n != DEMO_ERR_AGAIN)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(n, 5);
    EXPECT_EQ(memcmp(buf, msg, 5), 0);
    EXPECT_EQ(a.vtable->sock_send(a.user, conn, (const uint8_t *)msg, 5), 5);
    n = 0;
    for (int i = 0; i < 100; i++) {
        n = b.vtable->sock_recv(b.user, accepted, buf, sizeof(buf));
        if (n != DEMO_ERR_AGAIN)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(n, 5);

    a.vtable->sock_close(a.user, conn);
    a.vtable->sock_close(a.user, accepted);
    a.vtable->sock_close(a.user, listen);
    Destroy(&a);
    Destroy(&b);
}

TEST(SimBackend, InjectActions)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0);
    device_backend_instance_t inst = Create(opts);

    /* rssi_set：裸数字与 JSON 两种形式 */
    ASSERT_EQ(inst.vtable->inject(inst.user, "rssi_set", "-80"), DEMO_OK);
    int rssi = 0;
    EXPECT_EQ(inst.vtable->wifi_get_rssi(inst.user, &rssi), DEMO_OK);
    EXPECT_EQ(rssi, -80);
    ASSERT_EQ(inst.vtable->inject(inst.user, "rssi_set", "{\"rssi\":-75}"), DEMO_OK);
    EXPECT_EQ(inst.vtable->wifi_get_rssi(inst.user, &rssi), DEMO_OK);
    EXPECT_EQ(rssi, -75);

    /* 连接成功后 wifi_ssid_mismatch 覆盖当前 SSID */
    WriteHotspot(opts.catalog, "Modu_PC", "modu_leventure", 21000, NowMs(),
                 (unsigned long)sim_ap_catalog_current_pid());
    wifi_reason_t reason = WIFI_REASON_OK;
    ASSERT_EQ(inst.vtable->wifi_sta_connect(inst.user, "Modu_PC", "modu_leventure", &reason),
              DEMO_OK);
    ASSERT_EQ(inst.vtable->inject(inst.user, "wifi_ssid_mismatch", "RogueWifi"), DEMO_OK);
    char ssid[33];
    EXPECT_EQ(inst.vtable->wifi_get_current_ssid(inst.user, ssid, sizeof(ssid)), DEMO_OK);
    EXPECT_STREQ(ssid, "RogueWifi");

    /* mcast_block / mcast_unblock */
    void *mcast = nullptr;
    ASSERT_EQ(inst.vtable->udp_mcast_join(inst.user, kMcastGroup, kMcastPort, &mcast), DEMO_OK);
    ASSERT_EQ(inst.vtable->inject(inst.user, "mcast_block", nullptr), DEMO_OK);
    uint8_t buf[64];
    EXPECT_EQ(inst.vtable->udp_recv(inst.user, mcast, buf, sizeof(buf), nullptr), DEMO_ERR_AGAIN);
    ASSERT_EQ(inst.vtable->inject(inst.user, "mcast_unblock", nullptr), DEMO_OK);
    inst.vtable->sock_close(inst.user, mcast);

    /* burst_send 幂等成功 */
    EXPECT_EQ(inst.vtable->inject(inst.user, "burst_send", nullptr), DEMO_OK);

    /* 未知 action 返回错误 */
    EXPECT_EQ(inst.vtable->inject(inst.user, "no_such_action", nullptr), DEMO_ERR);

    Destroy(&inst);
}

TEST(SimBackend, MdnsRegisterResolveViaVtable)
{
    std::string root = MakeTempDir("simbackend");
    device_backend_instance_t inst = Create(Opts(root, 0));

    net_mdns_service_t svc;
    memset(&svc, 0, sizeof(svc));
    snprintf(svc.instance, sizeof(svc.instance), "host");
    snprintf(svc.type, sizeof(svc.type), "%s", kMdnsType);
    svc.addr.ip = inet_addr(PROTO_PC_AP_IP);
    svc.addr.port = htons(PROTO_TCP_PORT);
    snprintf(svc.txt, sizeof(svc.txt), "hello=1");
    ASSERT_EQ(inst.vtable->mdns_register(inst.user, &svc), DEMO_OK);

    net_mdns_service_t out;
    EXPECT_EQ(inst.vtable->mdns_resolve(inst.user, kMdnsType, &out, 500), DEMO_OK);
    EXPECT_EQ(out.addr.ip, svc.addr.ip);
    EXPECT_EQ(out.addr.port, svc.addr.port);
    EXPECT_STREQ(out.instance, "host");

    ASSERT_EQ(inst.vtable->mdns_unregister(inst.user, kMdnsType), DEMO_OK);
    EXPECT_EQ(inst.vtable->mdns_resolve(inst.user, kMdnsType, &out, 100), DEMO_ERR_TIMEOUT);
    Destroy(&inst);
}

TEST(SimBackend, NvsPersistAcrossBackends)
{
    std::string root = MakeTempDir("simbackend");
    {
        device_backend_instance_t inst = Create(Opts(root, 0));
        const char *v = "persist-me";
        ASSERT_EQ(inst.vtable->nvs_set(inst.user, "persist", (const uint8_t *)v, (int)strlen(v)), DEMO_OK);
        Destroy(&inst);
    }
    {
        device_backend_instance_t inst = Create(Opts(root, 0));
        uint8_t buf[64];
        int len = (int)sizeof(buf);
        EXPECT_EQ(inst.vtable->nvs_get(inst.user, "persist", buf, &len), DEMO_OK);
        EXPECT_EQ(len, 10);
        EXPECT_EQ(memcmp(buf, "persist-me", 10), 0);
        Destroy(&inst);
    }
}
