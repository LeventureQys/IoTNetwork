#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "device_config.h"

namespace {

static void WriteTemp(const char *path, const char *content)
{
    std::filesystem::create_directories("run");
    FILE *f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fputs(content, f);
    fclose(f);
}

/* 构造相对配置目录：其绝对路径长度严格大于 127（覆盖旧 128 字节字段的截断点），
 * 同时不超出 Windows MAX_PATH 绝对化限制（自适应深度）。 */
static std::string DeepConfigDir()
{
    std::string base = std::filesystem::absolute("run").string();
    size_t add = base.size() >= 128 ? 0 : 128 - base.size() + 1;
    return "run/d" + std::string(add, 'x') + "/cfg";
}

static void NormalizeSlashes(std::string &s)
{
#ifdef _WIN32
    for (auto &c : s)
        if (c == '/')
            c = '\\';
#endif
}

} // namespace

TEST(DeviceConfig, Defaults)
{
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, nullptr, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_EQ(missing, 1);
    EXPECT_EQ(cfg.host_tcp_port, 5935);
    EXPECT_EQ(cfg.heartbeat_interval_ms, 10000);
    EXPECT_EQ(cfg.host_max_conn, 16);
    EXPECT_STREQ(cfg.target_ssid, "TactileFactory-2.4G");
    EXPECT_EQ(cfg.device_count, 1);
    EXPECT_EQ(cfg.duration_s, 0);
    EXPECT_STREQ(cfg.nvs_dir, "run");
}

TEST(DeviceConfig, LoadOverrides)
{
    const char *path = "run/test_devcfg_1.json";
    WriteTemp(path,
              "{\"host_tcp_port\": 7000, \"heartbeat_interval_ms\": 2000, "
              "\"duration_s\": 30}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(cfg.host_tcp_port, 7000);
    EXPECT_EQ(cfg.heartbeat_interval_ms, 2000);
    EXPECT_EQ(cfg.duration_s, 30);
    EXPECT_EQ(cfg.host_max_conn, 16);
    EXPECT_STREQ(cfg.target_ssid, "TactileFactory-2.4G");
    std::remove(path);
}

TEST(DeviceConfig, MissingFileKeepsDefaults)
{
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, "run/not_exist_xyz.json", dir, sizeof(dir),
                                 &missing),
              DEMO_OK);
    EXPECT_EQ(missing, 1);
    EXPECT_EQ(cfg.host_tcp_port, 5935);
}

TEST(DeviceConfig, InvalidJson)
{
    const char *path = "run/test_devcfg_2.json";
    WriteTemp(path, "{invalid json");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_ERR);
    std::remove(path);
}

TEST(DeviceConfig, TypeMismatchIgnored)
{
    /* 字符串赋给 int 字段 → 忽略，保持默认（历史容错语义） */
    const char *path = "run/test_devcfg_3.json";
    WriteTemp(path, "{\"heartbeat_interval_ms\": \"oops\", \"host_max_conn\": 42}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_EQ(cfg.heartbeat_interval_ms, 10000); /* 默认值未被字符串破坏 */
    EXPECT_EQ(cfg.host_max_conn, 42);
    std::remove(path);
}

TEST(DeviceConfig, LongStringTruncated)
{
    const char *path = "run/test_devcfg_4.json";
    WriteTemp(path,
              "{\"target_ssid\": \"0123456789012345678901234567890123456789\"}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_EQ(strlen(cfg.target_ssid), sizeof(cfg.target_ssid) - 1);
    EXPECT_EQ(strncmp(cfg.target_ssid, "01234567890123456789012345678901", 32), 0);
    std::remove(path);
}

TEST(DeviceConfig, StringOverride)
{
    const char *path = "run/test_devcfg_5.json";
    WriteTemp(path, "{\"target_ssid\": \"Factory-5G\", \"host_virtual_ip\": \"10.0.0.9\"}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_STREQ(cfg.target_ssid, "Factory-5G");
    EXPECT_STREQ(cfg.host_virtual_ip, "10.0.0.9");
    std::remove(path);
}

TEST(DeviceConfig, LegacyHotspotConfigPath)
{
    const char *path = "run/test_devcfg_6.json";
    WriteTemp(path, "{\"linux_hotspot_config\": \"my/hs.json\"}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    std::string expect = (std::filesystem::absolute("run") / "my" / "hs.json").string();
    EXPECT_STREQ(cfg.hs_config_path, expect.c_str());
    std::remove(path);
}

TEST(DeviceConfig, ConfigDirReported)
{
    const char *path = "run/sub/test_devcfg_7.json";
    std::filesystem::create_directories("run/sub");
    WriteTemp(path, "{}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    std::string expect = std::filesystem::absolute("run/sub").string();
#ifdef _WIN32
    for (auto &c : expect)
        if (c == '/')
            c = '\\';
#endif
    EXPECT_STREQ(dir, expect.c_str());
    std::remove(path);
}

TEST(DeviceConfig, RelativeNvsDirResolvedAgainstConfigDir)
{
    const char *path = "run/test_devcfg_8.json";
    WriteTemp(path, "{\"nvs_dir\": \"mynvs\"}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    std::string expect = std::filesystem::absolute("run/mynvs").string();
#ifdef _WIN32
    for (auto &c : expect)
        if (c == '/')
            c = '\\';
#endif
    EXPECT_STREQ(cfg.nvs_dir, expect.c_str());
    std::remove(path);
}

TEST(DeviceConfig, AbsoluteNvsDirKept)
{
    const char *path = "run/test_devcfg_9.json";
#ifdef _WIN32
    const char *absolute_dir = "C:/some/abs/dir";
#else
    const char *absolute_dir = "/some/abs/dir";
#endif
    WriteTemp(path, (std::string("{\"nvs_dir\": \"") + absolute_dir + "\"}").c_str());
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
#ifdef _WIN32
    EXPECT_STREQ(cfg.nvs_dir, "C:\\some\\abs\\dir");
#else
    EXPECT_STREQ(cfg.nvs_dir, "/some/abs/dir");
#endif
    std::remove(path);
}

/* 缺陷修复回归：深目录（绝对路径 >127 字符）下相对 nvs_dir/hs_config_path
 * 按配置目录解析后完整保留，不再被旧 128 字节字段静默截断。 */
TEST(DeviceConfig, DeepRelativeNvsDirFullPathKept)
{
    std::string deep = DeepConfigDir();
    std::filesystem::create_directories(deep);
    std::string path = deep + "/devcfg_deep_nvs.json";
    WriteTemp(path.c_str(), "{\"nvs_dir\": \"mynvs\"}");
    device_config_t cfg;
    char dir[512];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path.c_str(), dir, sizeof(dir), &missing),
              DEMO_OK);
    std::string expect = (std::filesystem::absolute(deep) / "mynvs").string();
    NormalizeSlashes(expect);
    ASSERT_GT(expect.size(), 127u);
    EXPECT_STREQ(cfg.nvs_dir, expect.c_str());
    std::filesystem::remove_all(deep);
}

TEST(DeviceConfig, DeepRelativeHsConfigPathFullPathKept)
{
    std::string deep = DeepConfigDir();
    std::filesystem::create_directories(deep);
    std::string path = deep + "/devcfg_deep_hs.json";
    WriteTemp(path.c_str(), "{\"hs_config_path\": \"linux/hotspot.json\"}");
    device_config_t cfg;
    char dir[512];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path.c_str(), dir, sizeof(dir), &missing),
              DEMO_OK);
    std::string expect =
        (std::filesystem::absolute(deep) / "linux" / "hotspot.json").string();
    NormalizeSlashes(expect);
    ASSERT_GT(expect.size(), 127u);
    EXPECT_STREQ(cfg.hs_config_path, expect.c_str());
    std::filesystem::remove_all(deep);
}

TEST(DeviceConfig, DeepRelativeLegacyHotspotPathKept)
{
    std::string deep = DeepConfigDir();
    std::filesystem::create_directories(deep);
    std::string path = deep + "/devcfg_deep_legacy.json";
    WriteTemp(path.c_str(), "{\"linux_hotspot_config\": \"legacy/hs.json\"}");
    device_config_t cfg;
    char dir[512];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path.c_str(), dir, sizeof(dir), &missing),
              DEMO_OK);
    std::string expect = (std::filesystem::absolute(deep) / "legacy" / "hs.json").string();
    NormalizeSlashes(expect);
    ASSERT_GT(expect.size(), 127u);
    EXPECT_STREQ(cfg.hs_config_path, expect.c_str());
    std::filesystem::remove_all(deep);
}

TEST(DeviceConfig, DeepAbsolutePathKept)
{
    std::string abs;
#ifdef _WIN32
    abs = "C:/" + std::string(140, 'a') + "/nvs";
#else
    abs = "/" + std::string(140, 'a') + "/nvs";
#endif
    ASSERT_GT(abs.size(), 127u);
    std::string deep = DeepConfigDir();
    std::filesystem::create_directories(deep);
    std::string path = deep + "/devcfg_deep_abs.json";
    WriteTemp(path.c_str(), ("{\"nvs_dir\": \"" + abs + "\"}").c_str());
    device_config_t cfg;
    char dir[512];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path.c_str(), dir, sizeof(dir), &missing),
              DEMO_OK);
    std::string expect = abs;
    NormalizeSlashes(expect);
    EXPECT_STREQ(cfg.nvs_dir, expect.c_str());
    EXPECT_GT(strlen(cfg.nvs_dir), 127u);
    std::filesystem::remove_all(deep);
}
