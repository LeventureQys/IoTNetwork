#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include "sim_ap_catalog.h"

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

sim_ap_catalog_record_t MakeRecord(unsigned index, const char *ssid, unsigned port,
                                   uint64_t published_ms, unsigned long pid)
{
    sim_ap_catalog_record_t r;
    memset(&r, 0, sizeof(r));
    r.schema = SIM_AP_CATALOG_SCHEMA;
    r.device_index = index;
    sim_ap_catalog_build_device_id(index, r.device_id, sizeof(r.device_id));
    snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
    snprintf(r.bssid, sizeof(r.bssid), "%s", r.device_id);
    snprintf(r.logical_ip, sizeof(r.logical_ip), "192.168.1.1");
    snprintf(r.loopback_host, sizeof(r.loopback_host), "127.0.0.1");
    r.provision_port = port;
    r.published_at_ms = published_ms;
    r.owner_pid = pid;
    return r;
}

std::string FormalPath(const std::string &dir, unsigned index)
{
    return dir + "/device-" + std::to_string(index) + ".json";
}

void WriteRaw(const std::string &path, const char *text)
{
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fputs(text, fp);
    fclose(fp);
}

} // namespace

TEST(SimApCatalog, PublishReadRoundtrip)
{
    std::string dir = MakeTempDir("catalog");
    sim_ap_catalog_record_t rec = MakeRecord(0, "Modu_0001", 15935,
                                             sim_ap_catalog_wallclock_ms(),
                                             sim_ap_catalog_current_pid());
    EXPECT_EQ(sim_ap_catalog_publish(dir.c_str(), &rec), DEMO_OK);
    EXPECT_TRUE(std::filesystem::exists(FormalPath(dir, 0)));
    /* 无残留 tmp */
    bool has_tmp = false;
    for (const auto &e : std::filesystem::directory_iterator(dir))
        if (e.path().string().find(".tmp-") != std::string::npos)
            has_tmp = true;
    EXPECT_FALSE(has_tmp);

    sim_ap_catalog_record_t out;
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), 0, &out), DEMO_OK);
    EXPECT_EQ(out.schema, 1);
    EXPECT_EQ(out.device_index, 0u);
    EXPECT_STREQ(out.device_id, "02:00:00:00:00:01");
    EXPECT_STREQ(out.ssid, "Modu_0001");
    EXPECT_STREQ(out.bssid, "02:00:00:00:00:01");
    EXPECT_STREQ(out.logical_ip, "192.168.1.1");
    EXPECT_STREQ(out.loopback_host, "127.0.0.1");
    EXPECT_EQ(out.provision_port, 15935u);
    EXPECT_EQ(out.published_at_ms, rec.published_at_ms);
    EXPECT_EQ(out.owner_pid, rec.owner_pid);
}

TEST(SimApCatalog, UnknownFieldsIgnored)
{
    std::string dir = MakeTempDir("catalog");
    sim_ap_catalog_record_t rec = MakeRecord(1, "Modu_0002", 15936, 1000, 999999999);
    EXPECT_EQ(sim_ap_catalog_publish(dir.c_str(), &rec), DEMO_OK);
    /* 追加未知字段仍合法 */
    WriteRaw(FormalPath(dir, 1).c_str(),
             "{\"schema\":1,\"device_index\":1,\"device_id\":\"02:00:00:00:00:02\","
             "\"ssid\":\"Modu_0002\",\"bssid\":\"02:00:00:00:00:02\","
             "\"logical_ip\":\"192.168.1.1\",\"loopback_host\":\"127.0.0.1\","
             "\"provision_port\":15936,\"published_at_ms\":1000,\"owner_pid\":999999999,"
             "\"password\":\"hack\",\"pin\":\"x\"}");
    sim_ap_catalog_record_t out;
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), 1, &out), DEMO_OK);
    EXPECT_STREQ(out.ssid, "Modu_0002");
}

TEST(SimApCatalog, CorruptedAndInvalidRecordsIgnored)
{
    std::string dir = MakeTempDir("catalog");
    /* 垃圾文件 */
    WriteRaw(FormalPath(dir, 0).c_str(), "### not json ###");
    /* schema 非 1 */
    WriteRaw(FormalPath(dir, 1).c_str(),
             "{\"schema\":2,\"device_index\":1,\"device_id\":\"02:00:00:00:00:02\","
             "\"ssid\":\"A\",\"bssid\":\"02:00:00:00:00:02\",\"logical_ip\":\"192.168.1.1\","
             "\"loopback_host\":\"127.0.0.1\",\"provision_port\":15936,"
             "\"published_at_ms\":1000,\"owner_pid\":999999999}");
    /* 索引越界 */
    WriteRaw(FormalPath(dir, 2).c_str(),
             "{\"schema\":1,\"device_index\":99,\"device_id\":\"02:00:00:00:00:02\","
             "\"ssid\":\"B\",\"bssid\":\"02:00:00:00:00:02\",\"logical_ip\":\"192.168.1.1\","
             "\"loopback_host\":\"127.0.0.1\",\"provision_port\":15936,"
             "\"published_at_ms\":1000,\"owner_pid\":999999999}");
    /* 端口越界（0） */
    WriteRaw(FormalPath(dir, 3).c_str(),
             "{\"schema\":1,\"device_index\":3,\"device_id\":\"02:00:00:00:00:02\","
             "\"ssid\":\"C\",\"bssid\":\"02:00:00:00:00:02\",\"logical_ip\":\"192.168.1.1\","
             "\"loopback_host\":\"127.0.0.1\",\"provision_port\":0,"
             "\"published_at_ms\":1000,\"owner_pid\":999999999}");
    /* IP 非法 */
    WriteRaw(FormalPath(dir, 4).c_str(),
             "{\"schema\":1,\"device_index\":4,\"device_id\":\"02:00:00:00:00:02\","
             "\"ssid\":\"D\",\"bssid\":\"02:00:00:00:00:02\",\"logical_ip\":\"999.1.1.1\","
             "\"loopback_host\":\"127.0.0.1\",\"provision_port\":15936,"
             "\"published_at_ms\":1000,\"owner_pid\":999999999}");
    /* 缺字段 */
    WriteRaw(FormalPath(dir, 5).c_str(),
             "{\"schema\":1,\"device_index\":5,\"ssid\":\"E\",\"bssid\":\"x\","
             "\"logical_ip\":\"192.168.1.1\",\"loopback_host\":\"127.0.0.1\","
             "\"provision_port\":15936,\"published_at_ms\":1000,\"owner_pid\":999999999}");

    int count = 0;
    EXPECT_EQ(sim_ap_catalog_list(dir.c_str(), nullptr, 0, &count, 1), DEMO_OK);
    EXPECT_EQ(count, 0);
}

TEST(SimApCatalog, TtlAndOwnerPidExpiry)
{
    std::string dir = MakeTempDir("catalog");
    uint64_t now = sim_ap_catalog_wallclock_ms();
    unsigned long dead_pid = 999999999; /* 必然不存在的 PID */
    /* 新鲜（<30s）→ 有效，即使 pid 已死 */
    sim_ap_catalog_record_t fresh = MakeRecord(0, "Modu_Fresh", 21000, now - 1000, dead_pid);
    EXPECT_EQ(sim_ap_catalog_publish(dir.c_str(), &fresh), DEMO_OK);
    /* 过期且 pid 不存在 → 忽略 */
    sim_ap_catalog_record_t stale = MakeRecord(1, "Modu_Stale", 21001, now - 40000, dead_pid);
    EXPECT_EQ(sim_ap_catalog_publish(dir.c_str(), &stale), DEMO_OK);
    /* 过期但 pid 存活（本进程）→ 保留 */
    sim_ap_catalog_record_t alive = MakeRecord(2, "Modu_Alive", 21002, now - 40000,
                                               sim_ap_catalog_current_pid());
    EXPECT_EQ(sim_ap_catalog_publish(dir.c_str(), &alive), DEMO_OK);

    sim_ap_catalog_record_t records[16];
    int count = 0;
    EXPECT_EQ(sim_ap_catalog_list(dir.c_str(), records, 16, &count, now), DEMO_OK);
    EXPECT_EQ(count, 2);
    bool saw_fresh = false, saw_alive = false;
    for (int i = 0; i < count; i++) {
        if (records[i].device_index == 0)
            saw_fresh = true;
        if (records[i].device_index == 2)
            saw_alive = true;
    }
    EXPECT_TRUE(saw_fresh);
    EXPECT_TRUE(saw_alive);
}

TEST(SimApCatalog, AtomicReplaceVisibleToReaders)
{
    std::string dir = MakeTempDir("catalog");
    /* 先发布一条初始记录，保证读者首读必定命中 */
    sim_ap_catalog_record_t init = MakeRecord(3, "Modu_E", 21000,
                                              sim_ap_catalog_wallclock_ms(),
                                              sim_ap_catalog_current_pid());
    ASSERT_EQ(sim_ap_catalog_publish(dir.c_str(), &init), DEMO_OK);

    std::atomic<bool> stop{false};
    std::atomic<int> publish_errors{0};

    std::thread writer([&]() {
        for (int i = 0; i < 40 && !stop.load(); i++) {
            unsigned port = (i % 2 == 0) ? 21000 : 21001;
            const char *ssid = (i % 2 == 0) ? "Modu_E" : "Modu_O";
            sim_ap_catalog_record_t rec =
                MakeRecord(3, ssid, port, sim_ap_catalog_wallclock_ms(),
                           sim_ap_catalog_current_pid());
            if (sim_ap_catalog_publish(dir.c_str(), &rec) != DEMO_OK)
                publish_errors++;
        }
    });
    int read_errors = 0;
    for (int i = 0; i < 200; i++) {
        sim_ap_catalog_record_t rec;
        if (sim_ap_catalog_read(dir.c_str(), 3, &rec) != DEMO_OK) {
            read_errors++;
            continue;
        }
        /* 原子替换保证读到完整一致的一条记录，绝不混合两个版本 */
        bool pair_ok = (strcmp(rec.ssid, "Modu_E") == 0 && rec.provision_port == 21000) ||
                       (strcmp(rec.ssid, "Modu_O") == 0 && rec.provision_port == 21001);
        EXPECT_TRUE(pair_ok);
        EXPECT_EQ(rec.schema, SIM_AP_CATALOG_SCHEMA);
        EXPECT_EQ(rec.device_index, 3u);
    }
    stop = true;
    writer.join();
    EXPECT_EQ(publish_errors, 0);
    EXPECT_EQ(read_errors, 0);
}

TEST(SimApCatalog, ConcurrentPublishList)
{
    std::string dir = MakeTempDir("catalog");
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};

    std::thread writer([&]() {
        for (int i = 0; i < 30 && !stop.load(); i++) {
            sim_ap_catalog_record_t rec =
                MakeRecord(0, "Modu_Conc", 22000, sim_ap_catalog_wallclock_ms(),
                           sim_ap_catalog_current_pid());
            if (sim_ap_catalog_publish(dir.c_str(), &rec) != DEMO_OK)
                errors++;
        }
    });
    for (int i = 0; i < 100; i++) {
        int count = -1;
        if (sim_ap_catalog_list(dir.c_str(), nullptr, 0, &count, 0) != DEMO_OK) {
            errors++;
            continue;
        }
        if (count != 0 && count != 1)
            errors++;
    }
    stop = true;
    writer.join();
    EXPECT_EQ(errors, 0);
    /* 正式文件最终有效 */
    sim_ap_catalog_record_t out;
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), 0, &out), DEMO_OK);
    EXPECT_STREQ(out.ssid, "Modu_Conc");
}

TEST(SimApCatalog, RemoveDeletesFormalAndOwnTmp)
{
    std::string dir = MakeTempDir("catalog");
    sim_ap_catalog_record_t rec = MakeRecord(4, "Modu_R", 23000,
                                             sim_ap_catalog_wallclock_ms(),
                                             sim_ap_catalog_current_pid());
    EXPECT_EQ(sim_ap_catalog_publish(dir.c_str(), &rec), DEMO_OK);
    EXPECT_TRUE(std::filesystem::exists(FormalPath(dir, 4)));
    EXPECT_EQ(sim_ap_catalog_remove(dir.c_str(), 4, sim_ap_catalog_current_pid()), DEMO_OK);
    EXPECT_FALSE(std::filesystem::exists(FormalPath(dir, 4)));
    /* 重复删除幂等 */
    EXPECT_EQ(sim_ap_catalog_remove(dir.c_str(), 4, sim_ap_catalog_current_pid()), DEMO_OK);
}

TEST(SimApCatalog, DirectoryNotWritable)
{
    std::string dir = MakeTempDir("catalog");
    std::string blocker = dir + "/blocker";
    {
        FILE *fp = fopen(blocker.c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        fputs("x", fp);
        fclose(fp);
    }
    sim_ap_catalog_record_t rec = MakeRecord(0, "Modu_X", 24000,
                                             sim_ap_catalog_wallclock_ms(),
                                             sim_ap_catalog_current_pid());
    EXPECT_EQ(sim_ap_catalog_publish((blocker + "/cat").c_str(), &rec), DEMO_ERR);
    /* 失败后无 tmp 残留（blocker 是文件，无法落 tmp） */
}

TEST(SimApCatalog, DeviceIdDerivation)
{
    char buf[18];
    sim_ap_catalog_build_device_id(0, buf, sizeof(buf));
    EXPECT_STREQ(buf, "02:00:00:00:00:01");
    sim_ap_catalog_build_device_id(15, buf, sizeof(buf));
    EXPECT_STREQ(buf, "02:00:00:00:00:10");
    sim_ap_catalog_build_device_id(1, buf, sizeof(buf));
    EXPECT_STREQ(buf, "02:00:00:00:00:02");
}

TEST(SimApCatalog, FindSsid)
{
    std::string dir = MakeTempDir("catalog");
    sim_ap_catalog_record_t rec = MakeRecord(0, "Modu_One", 25000,
                                             sim_ap_catalog_wallclock_ms(),
                                             sim_ap_catalog_current_pid());
    EXPECT_EQ(sim_ap_catalog_publish(dir.c_str(), &rec), DEMO_OK);
    sim_ap_catalog_record_t out;
    EXPECT_EQ(sim_ap_catalog_find_ssid(dir.c_str(), "Modu_One", &out, 0), DEMO_OK);
    EXPECT_EQ(out.device_index, 0u);
    EXPECT_EQ(sim_ap_catalog_find_ssid(dir.c_str(), "NoSuch", &out, 0), DEMO_ERR);
}
