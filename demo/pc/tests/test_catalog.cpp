#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include "sim_backend.h"
#include "sim_world.h"
#include "params.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {

std::string now_ms_str()
{
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    return std::to_string(ms);
}

void write_catalog_file(const char *dir, const char *name, const char *content)
{
    std::filesystem::create_directories(dir);
    std::string path = std::string(dir) + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fputs(content, f);
    fclose(f);
}

std::string record_json(const char *ssid, int index, int port, const char *logical_ip,
                        long long published_ms, long long owner_pid)
{
    char device_id[32];
    snprintf(device_id, sizeof(device_id), "02:00:00:00:00:%02d", index + 1);
    return std::string("{\"schema\":1,\"device_index\":") + std::to_string(index) +
           ",\"device_id\":\"" + device_id +
           "\",\"ssid\":\"" + ssid + "\",\"bssid\":\"02:00:00:00:00:0" +
           std::to_string(index % 10) + "\",\"logical_ip\":\"" + logical_ip +
           "\",\"loopback_host\":\"127.0.0.1\",\"provision_port\":" + std::to_string(port) +
           ",\"published_at_ms\":" + std::to_string(published_ms) +
           ",\"owner_pid\":" + std::to_string(owner_pid) + "}";
}

const char *kCatalogDir = "run/cat_test";

} // namespace

TEST(SimCatalog, ReadsFormalFilesSkipsTmpAndInvalid)
{
    std::filesystem::remove_all(kCatalogDir);
    std::string fresh = now_ms_str();
    /* 正式文件：最新，有效 */
    write_catalog_file(kCatalogDir, "device-0.json",
                       record_json("Modu_0001", 0, 21000, "192.168.1.1",
                                   std::stoll(fresh), getpid()).c_str());
    /* 临时文件：跳过 */
    write_catalog_file(kCatalogDir, "device-1.json.tmp-1234",
                       record_json("Modu_0002", 1, 21001, "192.168.1.1",
                                   std::stoll(fresh), getpid()).c_str());
    /* schema 非 1：忽略 */
    write_catalog_file(kCatalogDir, "device-2.json",
                       "{\"schema\":2,\"device_index\":2,\"device_id\":\"x\",\"ssid\":\"Modu_0003\","
                       "\"bssid\":\"x\",\"logical_ip\":\"192.168.1.1\",\"loopback_host\":\"127.0.0.1\","
                       "\"provision_port\":21002,\"published_at_ms\":1,\"owner_pid\":0}");
    /* 索引越界文件名：忽略 */
    write_catalog_file(kCatalogDir, "device-16.json",
                       record_json("Modu_0004", 16, 21003, "192.168.1.1",
                                   std::stoll(fresh), getpid()).c_str());
    /* 非法 IP：忽略 */
    write_catalog_file(kCatalogDir, "device-3.json",
                       "{\"schema\":1,\"device_index\":3,\"device_id\":\"x\",\"ssid\":\"Modu_0005\","
                       "\"bssid\":\"x\",\"logical_ip\":\"999.1.1.1\",\"loopback_host\":\"127.0.0.1\","
                       "\"provision_port\":21004,\"published_at_ms\":1,\"owner_pid\":0}");
    /* 损坏 JSON：忽略 */
    write_catalog_file(kCatalogDir, "device-4.json", "{broken");
    /* 非契约文件名：忽略 */
    write_catalog_file(kCatalogDir, "other.json",
                       record_json("Modu_0006", 0, 21005, "192.168.1.1",
                                   std::stoll(fresh), getpid()).c_str());

    demo_params_t params;
    params_defaults(&params);
    snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "%s", kCatalogDir);
    sim_ap_record_t records[16];
    int count = sim_backend_ap_list(&params, records, 16);
    ASSERT_EQ(count, 1);
    EXPECT_STREQ(records[0].ssid, "Modu_0001");
    EXPECT_EQ(records[0].real_port, 21000);
    EXPECT_STREQ(records[0].device_id, "02:00:00:00:00:01");
    /* 新契约无密码/PIN：PC 使用固定演示凭据 */
    EXPECT_STREQ(records[0].password, "modutech_leventure");
    EXPECT_STREQ(records[0].pin, "5935");
    std::filesystem::remove_all(kCatalogDir);
}

TEST(SimCatalog, ExpiredWhenOldAndPidDead)
{
    std::filesystem::remove_all(kCatalogDir);
    /* 陈旧（published_at_ms=1）且 owner_pid=0 不存在 → 过期忽略 */
    write_catalog_file(kCatalogDir, "device-0.json",
                       record_json("Modu_OLD", 0, 21000, "192.168.1.1", 1, 0).c_str());
    demo_params_t params;
    params_defaults(&params);
    snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "%s", kCatalogDir);
    sim_ap_record_t records[16];
    EXPECT_EQ(sim_backend_ap_list(&params, records, 16), 0);
    std::filesystem::remove_all(kCatalogDir);
}

TEST(SimCatalog, OldButOwnerAliveStillValid)
{
    std::filesystem::remove_all(kCatalogDir);
    /* 陈旧但 owner_pid=当前进程（存活）→ 仍有效 */
    write_catalog_file(kCatalogDir, "device-0.json",
                       record_json("Modu_ALIVE", 0, 21000, "192.168.1.1", 1, getpid()).c_str());
    demo_params_t params;
    params_defaults(&params);
    snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "%s", kCatalogDir);
    sim_ap_record_t records[16];
    int count = sim_backend_ap_list(&params, records, 16);
    ASSERT_EQ(count, 1);
    EXPECT_STREQ(records[0].ssid, "Modu_ALIVE");
    std::filesystem::remove_all(kCatalogDir);
}

TEST(SimCatalog, FindBySsid)
{
    std::filesystem::remove_all(kCatalogDir);
    std::string fresh = now_ms_str();
    write_catalog_file(kCatalogDir, "device-5.json",
                       record_json("Modu_0009", 5, 21009, "192.168.1.1",
                                   std::stoll(fresh), getpid()).c_str());
    demo_params_t params;
    params_defaults(&params);
    snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "%s", kCatalogDir);
    sim_ap_record_t record;
    EXPECT_EQ(sim_backend_ap_find(&params, "Modu_0009", &record), DEMO_OK);
    EXPECT_STREQ(record.ssid, "Modu_0009");
    EXPECT_EQ(record.real_port, 21009);
    EXPECT_NE(sim_backend_ap_find(&params, "Modu_MISSING", &record), DEMO_OK);
    std::filesystem::remove_all(kCatalogDir);
}

TEST(SimCatalog, MissingDirectoryReturnsEmpty)
{
    std::filesystem::remove_all(kCatalogDir);
    demo_params_t params;
    params_defaults(&params);
    snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "%s", kCatalogDir);
    sim_ap_record_t records[16];
    EXPECT_EQ(sim_backend_ap_list(&params, records, 16), 0);
}
