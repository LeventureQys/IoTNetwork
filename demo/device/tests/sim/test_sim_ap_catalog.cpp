#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
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

std::string FormalPath(const std::string &dir)
{
    return dir + "/pc-hotspot.json";
}

void WriteRaw(const std::string &path, const char *text)
{
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fputs(text, fp);
    fclose(fp);
}

std::string HotspotJson(const char *ssid, const char *password, unsigned loopback_port,
                        uint64_t published_ms, unsigned long pid)
{
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"schema\":2,\"ssid\":\"%s\",\"password\":\"%s\","
             "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
             "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
             "\"loopback_port\":%u,\"published_at_ms\":%llu,\"owner_pid\":%lu}",
             ssid, password, loopback_port,
             (unsigned long long)published_ms, pid);
    return buf;
}

} // namespace

TEST(SimApCatalog, ReadRoundtrip)
{
    std::string dir = MakeTempDir("catalog");
    uint64_t now = sim_ap_catalog_wallclock_ms();
    unsigned long pid = sim_ap_catalog_current_pid();
    WriteRaw(FormalPath(dir).c_str(),
             HotspotJson("Modu_PC", "modu_leventure", 15935, now, pid).c_str());

    sim_ap_catalog_record_t out;
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 0), DEMO_OK);
    EXPECT_EQ(out.schema, 2);
    EXPECT_STREQ(out.ssid, "Modu_PC");
    EXPECT_STREQ(out.password, "modu_leventure");
    EXPECT_STREQ(out.logical_gateway, "192.168.137.1");
    EXPECT_EQ(out.prefix_length, 24);
    EXPECT_EQ(out.tcp_port, 5935u);
    EXPECT_STREQ(out.loopback_host, "127.0.0.1");
    EXPECT_EQ(out.loopback_port, 15935u);
    EXPECT_EQ(out.published_at_ms, now);
    EXPECT_EQ(out.owner_pid, pid);
}

TEST(SimApCatalog, UnknownFieldsIgnored)
{
    std::string dir = MakeTempDir("catalog");
    /* 追加未知字段仍合法（前向兼容，PC 端后续可增加字段） */
    const char *json =
        "{\"schema\":2,\"ssid\":\"Modu_PC\",\"password\":\"modu_leventure\","
        "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
        "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
        "\"loopback_port\":15936,\"published_at_ms\":1000,\"owner_pid\":999999999,"
        "\"bssid\":\"aa:bb:cc:dd:ee:ff\",\"extra\":\"x\"}";
    WriteRaw(FormalPath(dir).c_str(), json);

    sim_ap_catalog_record_t out;
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 1), DEMO_OK);
    EXPECT_STREQ(out.ssid, "Modu_PC");
}

TEST(SimApCatalog, CorruptedAndInvalidRecordsIgnored)
{
    std::string dir = MakeTempDir("catalog");

    /* 1) 垃圾 JSON → 忽略 */
    WriteRaw(FormalPath(dir).c_str(), "### not json ###");
    sim_ap_catalog_record_t out;
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 1), DEMO_ERR);

    /* 2) schema 非 2 → 忽略 */
    WriteRaw(FormalPath(dir).c_str(),
             "{\"schema\":1,\"ssid\":\"A\",\"password\":\"p\","
             "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
             "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
             "\"loopback_port\":1,\"published_at_ms\":1,\"owner_pid\":1}");
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 1), DEMO_ERR);

    /* 3) 缺 password 字段 → 忽略 */
    WriteRaw(FormalPath(dir).c_str(),
             "{\"schema\":2,\"ssid\":\"B\","
             "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
             "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
             "\"loopback_port\":1,\"published_at_ms\":1,\"owner_pid\":1}");
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 1), DEMO_ERR);

    /* 4) logical_gateway 非法 IP → 忽略 */
    WriteRaw(FormalPath(dir).c_str(),
             "{\"schema\":2,\"ssid\":\"C\",\"password\":\"p\","
             "\"logical_gateway\":\"999.1.1.1\",\"prefix_length\":24,"
             "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
             "\"loopback_port\":1,\"published_at_ms\":1,\"owner_pid\":1}");
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 1), DEMO_ERR);

    /* 5) loopback_port 越界（0） → 忽略 */
    WriteRaw(FormalPath(dir).c_str(),
             "{\"schema\":2,\"ssid\":\"D\",\"password\":\"p\","
             "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
             "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
             "\"loopback_port\":0,\"published_at_ms\":1,\"owner_pid\":1}");
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 1), DEMO_ERR);

    /* 6) prefix_length 越界（33） → 忽略 */
    WriteRaw(FormalPath(dir).c_str(),
             "{\"schema\":2,\"ssid\":\"E\",\"password\":\"p\","
             "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":33,"
             "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\","
             "\"loopback_port\":1,\"published_at_ms\":1,\"owner_pid\":1}");
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 1), DEMO_ERR);
}

TEST(SimApCatalog, TtlAndOwnerPidExpiry)
{
    uint64_t now = sim_ap_catalog_wallclock_ms();
    unsigned long dead_pid = 999999999; /* 必然不存在的 PID */

    /* 新鲜（<30s）→ 有效，即使 pid 已死 */
    {
        std::string dir = MakeTempDir("catalog");
        WriteRaw(FormalPath(dir).c_str(),
                 HotspotJson("Modu_Fresh", "p", 21000, now - 1000, dead_pid).c_str());
        sim_ap_catalog_record_t out;
        EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, now), DEMO_OK);
        EXPECT_STREQ(out.ssid, "Modu_Fresh");
    }
    /* 过期（>30s）且 pid 不存在 → 忽略 */
    {
        std::string dir = MakeTempDir("catalog");
        WriteRaw(FormalPath(dir).c_str(),
                 HotspotJson("Modu_Stale", "p", 21001, now - 40000, dead_pid).c_str());
        sim_ap_catalog_record_t out;
        EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, now), DEMO_ERR);
    }
    /* 过期但 pid 存活（本进程）→ 保留 */
    {
        std::string dir = MakeTempDir("catalog");
        WriteRaw(FormalPath(dir).c_str(),
                 HotspotJson("Modu_Alive", "p", 21002, now - 40000,
                             sim_ap_catalog_current_pid()).c_str());
        sim_ap_catalog_record_t out;
        EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, now), DEMO_OK);
        EXPECT_STREQ(out.ssid, "Modu_Alive");
    }
}

TEST(SimApCatalog, BuildPath)
{
    char buf[1024];
    EXPECT_EQ(sim_ap_catalog_build_path("C:\\tmp\\cat", buf, sizeof(buf)), DEMO_OK);
    EXPECT_NE(std::string(buf).find("pc-hotspot.json"), std::string::npos);

    /* 目录为空 → INVAL */
    EXPECT_EQ(sim_ap_catalog_build_path(nullptr, buf, sizeof(buf)), DEMO_ERR_INVAL);
}

TEST(SimApCatalog, TmpFileNotRead)
{
    std::string dir = MakeTempDir("catalog");
    /* 只有 tmp 文件（发布者写了一半/崩溃）→ 读取不到正式记录 */
    WriteRaw((dir + "/pc-hotspot.json.tmp-123").c_str(),
             HotspotJson("Modu_PC", "p", 22000, 1, 1).c_str());
    sim_ap_catalog_record_t out;
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 1), DEMO_ERR);

    /* 正式文件与 tmp 并存 → 只读正式文件 */
    WriteRaw(FormalPath(dir).c_str(),
             HotspotJson("Modu_PC", "modu_leventure", 22001,
                         sim_ap_catalog_wallclock_ms(),
                         sim_ap_catalog_current_pid()).c_str());
    EXPECT_EQ(sim_ap_catalog_read(dir.c_str(), &out, 0), DEMO_OK);
    EXPECT_STREQ(out.ssid, "Modu_PC");
    EXPECT_EQ(out.loopback_port, 22001u);
}
