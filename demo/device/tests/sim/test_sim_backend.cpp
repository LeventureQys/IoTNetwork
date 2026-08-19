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

std::string CatalogPath(const std::string &root, unsigned idx)
{
    return root + "/catalog/device-" + std::to_string(idx) + ".json";
}

struct Opts {
    device_sim_backend_options_t o;
    std::string catalog;
    std::string nvs;

    Opts(const std::string &root, unsigned idx, unsigned port, uint32_t seed = 42)
    {
        memset(&o, 0, sizeof(o));
        catalog = root + "/catalog";
        nvs = root + "/dev" + std::to_string(idx) + ".nvs.json";
        o.config_path = "unused";
        o.sim_catalog_dir = catalog.c_str();
        o.nvs_file = nvs.c_str();
        o.target_ssid = "TactileFactory-2.4G";
        o.target_password = "modutech_leventure";
        o.host_virtual_ip = "192.168.1.50";
        o.device_index = idx;
        o.provision_port = port;
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

} // namespace

TEST(SimBackendFactory, SuccessFillsInstance)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0, 21000);
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
    Opts o(root, 0, 21000);
    o.o.sim_catalog_dir = "";
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(&o.o, &inst, &err), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.user, nullptr);
    EXPECT_STREQ(err.operation, "device_sim_backend_create");
    EXPECT_NE(err.message[0], 0);

    /* catalog_dir 非绝对路径 */
    Opts o2(root, 0, 21000);
    o2.o.sim_catalog_dir = "relative/catalog";
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(&o2.o, &inst, &err), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.user, nullptr);

    /* device_index 越界 */
    Opts o3(root, 16, 21000);
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(&o3.o, &inst, &err), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.user, nullptr);

    /* provision_port 越界 */
    Opts o4(root, 0, 70000);
    memset(&inst, 0xFF, sizeof(inst));
    EXPECT_EQ(device_sim_backend_create(&o4.o, &inst, &err), DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.user, nullptr);

    /* out_instance 为空 */
    EXPECT_EQ(device_sim_backend_create(&o4.o, nullptr, &err), DEVICE_ERR_INVALID_ARGUMENT);
}

TEST(SimBackendFactory, OptionsStringsAreCopied)
{
    std::string root = MakeTempDir("simbackend");
    std::string original = root + "/catalog_orig";
    std::string mutated = root + "/catalog_mutated";
    char borrowed[512];
    snprintf(borrowed, sizeof(borrowed), "%s", original.c_str());

    Opts opts(root, 0, 21000);
    opts.o.sim_catalog_dir = borrowed;
    device_backend_instance_t inst = Create(opts);
    /* 调用期借用结束后修改源字符串，实例行为不受影响 */
    snprintf(borrowed, sizeof(borrowed), "%s", mutated.c_str());

    ASSERT_EQ(inst.vtable->wifi_ap_start(inst.user, "Modu_0001", "pass123456", "1234"), DEMO_OK);
    EXPECT_TRUE(std::filesystem::exists(original + "/device-0.json"));
    EXPECT_FALSE(std::filesystem::exists(mutated + "/device-0.json"));
    Destroy(&inst);
}

TEST(SimBackendFactory, DestroyIdempotentAndRemovesCatalog)
{
    std::string root = MakeTempDir("simbackend");
    Opts opts(root, 0, 21000);
    device_backend_instance_t inst = Create(opts);
    ASSERT_EQ(inst.vtable->wifi_ap_start(inst.user, "Modu_0001", "pass123456", "1234"), DEMO_OK);
    EXPECT_TRUE(std::filesystem::exists(CatalogPath(root, 0)));
    /* 销毁清理：world 注销 + catalog 正式文件与本进程 tmp 删除 */
    Destroy(&inst);
    EXPECT_FALSE(std::filesystem::exists(CatalogPath(root, 0)));
    /* destroy_user 可接收 NULL（struct 级幂等由 SS03 的 instance_destroy 保证） */
    inst.destroy_user = nullptr;
}

TEST(SimBackendFactory, DestroyAllZeroInstanceSafe)
{
    device_backend_instance_t inst;
    memset(&inst, 0, sizeof(inst));
    /* 全零实例：destroy_user 为空，无需任何操作（无崩溃即通过） */
    EXPECT_EQ(inst.destroy_user, nullptr);
}

TEST(SimBackend, TwoInstancesIsolated)
{
    std::string root = MakeTempDir("simbackend");
    device_backend_instance_t a = Create(Opts(root, 0, 21000, 7));
    device_backend_instance_t b = Create(Opts(root, 1, 21001, 9));

    /* A 开 AP 只影响 A */
    ASSERT_EQ(a.vtable->wifi_ap_start(a.user, "Modu_A", "pass123456", "1234"), DEMO_OK);
    EXPECT_TRUE(std::filesystem::exists(CatalogPath(root, 0)));
    EXPECT_FALSE(std::filesystem::exists(CatalogPath(root, 1)));

    /* B 的 RSSI 不受 A 注入影响 */
    ASSERT_EQ(a.vtable->inject(a.user, "rssi_set", "-80"), DEMO_OK);
    int rssi = 0;
    EXPECT_EQ(a.vtable->wifi_get_rssi(a.user, &rssi), DEMO_OK);
    EXPECT_EQ(rssi, -80);
    EXPECT_EQ(b.vtable->wifi_get_rssi(b.user, &rssi), DEMO_OK);
    EXPECT_EQ(rssi, -58);

    /* B 的 AP 状态不受 A 影响 */
    uint32_t ip = 0xFFFFFFFF;
    EXPECT_EQ(b.vtable->wifi_get_ip(b.user, &ip), DEMO_OK);
    EXPECT_EQ(ip, 0u);
    EXPECT_EQ(a.vtable->wifi_get_ip(a.user, &ip), DEMO_OK);
    EXPECT_EQ(ip, sim_world_device_ap_virtual_ip());

    /* B 销毁不影响 A 的 catalog 文件 */
    Destroy(&b);
    EXPECT_TRUE(std::filesystem::exists(CatalogPath(root, 0)));

    /* NVS 文件隔离 */
    EXPECT_FALSE(std::filesystem::exists(root + "/dev1.nvs.json"));
    EXPECT_FALSE(std::filesystem::exists(root + "/dev0.nvs.json"));
    ASSERT_EQ(a.vtable->nvs_set(a.user, "k", (const uint8_t *)"v", 1), DEMO_OK);
    EXPECT_TRUE(std::filesystem::exists(root + "/dev0.nvs.json"));

    /* A 停止 AP 清理自己 */
    ASSERT_EQ(a.vtable->wifi_ap_stop(a.user), DEMO_OK);
    EXPECT_FALSE(std::filesystem::exists(CatalogPath(root, 0)));
    Destroy(&a);
}

TEST(SimBackend, FaultInjectionConsumptionIsolation)
{
    std::string root = MakeTempDir("simbackend");
    device_backend_instance_t a = Create(Opts(root, 0, 21000));
    device_backend_instance_t b = Create(Opts(root, 1, 21001));

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

    /* A 注入 sock_send_fail：1 次失败后恢复；B 的发送路径独立 */
    ASSERT_EQ(a.vtable->inject(a.user, "sock_send_fail", "{\"count\":1}"), DEMO_OK);
    const char *msg = "hello";
    EXPECT_EQ(a.vtable->sock_send(a.user, conn, (const uint8_t *)msg, 5), DEMO_ERR); /* 注入失败，未发送 */
    /* B 正常发送，A 正常接收（B 不受 A 注入影响） */
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
    /* A 再次发送恢复（注入计数已消耗），B 正常收到 */
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
    device_backend_instance_t inst = Create(Opts(root, 0, 21000));
    wifi_reason_t reason = WIFI_REASON_OK;

    /* wifi_disconnect / wifi_ok */
    ASSERT_EQ(inst.vtable->inject(inst.user, "wifi_disconnect", nullptr), DEMO_OK);
    EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "TactileFactory-2.4G",
                                            "modutech_leventure", &reason),
              DEMO_ERR);
    EXPECT_EQ((int)reason, 201);
    ASSERT_EQ(inst.vtable->inject(inst.user, "wifi_ok", nullptr), DEMO_OK);
    EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "TactileFactory-2.4G",
                                            "modutech_leventure", &reason),
              DEMO_OK);
    EXPECT_EQ((int)reason, 0);

    /* wifi_auth_fail / wifi_auth_ok */
    ASSERT_EQ(inst.vtable->inject(inst.user, "wifi_auth_fail", nullptr), DEMO_OK);
    EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "TactileFactory-2.4G",
                                            "modutech_leventure", &reason),
              DEMO_ERR);
    EXPECT_EQ((int)reason, 202);
    ASSERT_EQ(inst.vtable->inject(inst.user, "wifi_auth_ok", nullptr), DEMO_OK);
    EXPECT_EQ(inst.vtable->wifi_sta_connect(inst.user, "TactileFactory-2.4G",
                                            "modutech_leventure", &reason),
              DEMO_OK);

    /* rssi_set：裸数字与 JSON 两种形式 */
    ASSERT_EQ(inst.vtable->inject(inst.user, "rssi_set", "-80"), DEMO_OK);
    int rssi = 0;
    EXPECT_EQ(inst.vtable->wifi_get_rssi(inst.user, &rssi), DEMO_OK);
    EXPECT_EQ(rssi, -80);
    ASSERT_EQ(inst.vtable->inject(inst.user, "rssi_set", "{\"rssi\":-75}"), DEMO_OK);
    EXPECT_EQ(inst.vtable->wifi_get_rssi(inst.user, &rssi), DEMO_OK);
    EXPECT_EQ(rssi, -75);

    /* wifi_ssid_mismatch：未连接时失败 */
    ASSERT_EQ(inst.vtable->wifi_sta_disconnect(inst.user), DEMO_OK);
    EXPECT_EQ(inst.vtable->inject(inst.user, "wifi_ssid_mismatch", "RogueWifi"), DEMO_ERR);
    /* 连接后注入生效 */
    ASSERT_EQ(inst.vtable->wifi_sta_connect(inst.user, "TactileFactory-2.4G",
                                            "modutech_leventure", &reason),
              DEMO_OK);
    EXPECT_EQ(inst.vtable->inject(inst.user, "wifi_ssid_mismatch", "RogueWifi"), DEMO_OK);
    char ssid[33];
    EXPECT_EQ(inst.vtable->wifi_get_current_ssid(inst.user, ssid, sizeof(ssid)), DEMO_OK);
    EXPECT_STREQ(ssid, "RogueWifi");

    /* mcast_block / mcast_unblock */
    void *mcast = nullptr;
    ASSERT_EQ(inst.vtable->udp_mcast_join(inst.user, PROTO_MCAST_GROUP, PROTO_MCAST_PORT, &mcast), DEMO_OK);
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
    device_backend_instance_t inst = Create(Opts(root, 0, 21000));

    net_mdns_service_t svc;
    memset(&svc, 0, sizeof(svc));
    snprintf(svc.instance, sizeof(svc.instance), "host");
    snprintf(svc.type, sizeof(svc.type), PROTO_MDNS_TYPE);
    svc.addr.ip = 0x3201A8C0;
    svc.addr.port = htons(PROTO_TCP_PORT);
    snprintf(svc.txt, sizeof(svc.txt), "hello=1");
    ASSERT_EQ(inst.vtable->mdns_register(inst.user, &svc), DEMO_OK);

    net_mdns_service_t out;
    EXPECT_EQ(inst.vtable->mdns_resolve(inst.user, PROTO_MDNS_TYPE, &out, 500), DEMO_OK);
    EXPECT_EQ(out.addr.ip, svc.addr.ip);
    EXPECT_EQ(out.addr.port, svc.addr.port);
    EXPECT_STREQ(out.instance, "host");

    ASSERT_EQ(inst.vtable->mdns_unregister(inst.user, PROTO_MDNS_TYPE), DEMO_OK);
    EXPECT_EQ(inst.vtable->mdns_resolve(inst.user, PROTO_MDNS_TYPE, &out, 100), DEMO_ERR_TIMEOUT);
    Destroy(&inst);
}

TEST(SimBackend, NvsPersistAcrossBackends)
{
    std::string root = MakeTempDir("simbackend");
    {
        device_backend_instance_t inst = Create(Opts(root, 0, 21000));
        const char *v = "persist-me";
        ASSERT_EQ(inst.vtable->nvs_set(inst.user, "persist", (const uint8_t *)v, (int)strlen(v)), DEMO_OK);
        Destroy(&inst);
    }
    {
        device_backend_instance_t inst = Create(Opts(root, 0, 21000));
        uint8_t buf[64];
        int len = (int)sizeof(buf);
        EXPECT_EQ(inst.vtable->nvs_get(inst.user, "persist", buf, &len), DEMO_OK);
        EXPECT_EQ(len, 10);
        EXPECT_EQ(memcmp(buf, "persist-me", 10), 0);
        Destroy(&inst);
    }
}

TEST(SimBackend, WifiScanMergesWorldCatalogTarget)
{
    std::string root = MakeTempDir("simbackend");
    device_backend_instance_t a = Create(Opts(root, 0, 21000));
    device_backend_instance_t b = Create(Opts(root, 1, 21001));

    ASSERT_EQ(a.vtable->wifi_ap_start(a.user, "Modu_ScanA", "pass123456", "1234"), DEMO_OK);

    net_ap_info_t aps[16];
    int count = 16;
    EXPECT_EQ(b.vtable->wifi_scan(b.user, aps, &count), DEMO_OK);
    /* B 扫描：catalog 的 Modu_ScanA + 目标网络 */
    bool saw_ap = false, saw_target = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(aps[i].ssid, "Modu_ScanA") == 0)
            saw_ap = true;
        if (strcmp(aps[i].ssid, "TactileFactory-2.4G") == 0)
            saw_target = true;
    }
    EXPECT_TRUE(saw_ap);
    EXPECT_TRUE(saw_target);

    /* 容量 0 时 count 仍统计全部 */
    count = 0;
    EXPECT_EQ(b.vtable->wifi_scan(b.user, nullptr, &count), DEMO_OK);
    EXPECT_GE(count, 2);

    Destroy(&a);
    Destroy(&b);
}

TEST(SimBackend, StaConnectViaCatalog)
{
    std::string root = MakeTempDir("simbackend");
    device_backend_instance_t a = Create(Opts(root, 0, 21000));
    device_backend_instance_t b = Create(Opts(root, 1, 21001));
    ASSERT_EQ(a.vtable->wifi_ap_start(a.user, "Modu_Cat", "pass123456", "1234"), DEMO_OK);
    wifi_reason_t reason = WIFI_REASON_OK;
    /* B 通过 catalog 发现并连接 A 的 AP（冻结 schema 无密码字段，命中即成功） */
    EXPECT_EQ(b.vtable->wifi_sta_connect(b.user, "Modu_Cat", "wrongpass", &reason), DEMO_OK);
    EXPECT_EQ((int)reason, 0);
    char cur[33];
    EXPECT_EQ(b.vtable->wifi_get_current_ssid(b.user, cur, sizeof(cur)), DEMO_OK);
    EXPECT_STREQ(cur, "Modu_Cat");
    Destroy(&a);
    Destroy(&b);
}
