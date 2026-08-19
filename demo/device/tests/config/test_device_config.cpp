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
    EXPECT_STREQ(cfg.pc_ap_ssid, "Modu_PC");
    EXPECT_STREQ(cfg.pc_ap_password, "modu_leventure");
    EXPECT_STREQ(cfg.pc_host_ip, "192.168.137.1");
    EXPECT_EQ(cfg.wifi_retry_max, 5);
    EXPECT_EQ(cfg.use_real_wifi_sta, 0);
    EXPECT_EQ(cfg.duration_s, 0);
    EXPECT_STREQ(cfg.nvs_dir, "run");
}

TEST(DeviceConfig, LoadOverrides)
{
    const char *path = "run/test_devcfg_1.json";
    WriteTemp(path,
              "{\"host_tcp_port\": 5935, \"heartbeat_interval_ms\": 2000, "
              "\"duration_s\": 30, \"pc_ap_ssid\": \"Modu_Test\"}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(cfg.host_tcp_port, 5935);
    EXPECT_EQ(cfg.heartbeat_interval_ms, 2000);
    EXPECT_EQ(cfg.duration_s, 30);
    EXPECT_STREQ(cfg.pc_ap_ssid, "Modu_Test");
    EXPECT_STREQ(cfg.pc_host_ip, "192.168.137.1");
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
    WriteTemp(path, "{\"heartbeat_interval_ms\": \"oops\", \"wifi_retry_max\": 42}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_EQ(cfg.heartbeat_interval_ms, 10000); /* 默认值未被字符串破坏 */
    EXPECT_EQ(cfg.wifi_retry_max, 42);
    std::remove(path);
}

TEST(DeviceConfig, LongStringTruncated)
{
    const char *path = "run/test_devcfg_4.json";
    WriteTemp(path,
              "{\"pc_ap_ssid\": \"Modu_0123456789012345678901234567890123456789\"}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_EQ(strlen(cfg.pc_ap_ssid), sizeof(cfg.pc_ap_ssid) - 1);
    EXPECT_EQ(strncmp(cfg.pc_ap_ssid, "Modu_", 5), 0);
    std::remove(path);
}

TEST(DeviceConfig, StringOverride)
{
    const char *path = "run/test_devcfg_5.json";
    WriteTemp(path, "{\"pc_ap_ssid\": \"Modu_Factory\", \"pc_host_ip\": \"192.168.137.1\"}");
    device_config_t cfg;
    char dir[256];
    int missing = 0;
    EXPECT_EQ(device_config_load(&cfg, path, dir, sizeof(dir), &missing), DEMO_OK);
    EXPECT_STREQ(cfg.pc_ap_ssid, "Modu_Factory");
    EXPECT_STREQ(cfg.pc_host_ip, "192.168.137.1");
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

/* 缺陷修复回归：深目录（绝对路径 >127 字符）下相对 nvs_dir 按配置目录解析后完整保留。 */
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

/* ---------- device_config_validate 契约用例 ---------- */

TEST(DeviceConfigValidate, DefaultsOk)
{
    device_config_t cfg;
    device_config_defaults(&cfg);
    char err[128];
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_OK);
}

TEST(DeviceConfigValidate, ModuPcOk)
{
    device_config_t cfg;
    device_config_defaults(&cfg);
    snprintf(cfg.pc_ap_ssid, sizeof(cfg.pc_ap_ssid), "Modu_PC");
    char err[128];
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_OK);
}

TEST(DeviceConfigValidate, NullArgs)
{
    device_config_t cfg;
    device_config_defaults(&cfg);
    char err[128];
    EXPECT_EQ(device_config_validate(nullptr, err, sizeof(err)), DEMO_ERR_INVAL);
    EXPECT_EQ(device_config_validate(&cfg, nullptr, sizeof(err)), DEMO_ERR_INVAL);
    EXPECT_EQ(device_config_validate(&cfg, err, 0), DEMO_ERR_INVAL);
}

TEST(DeviceConfigValidate, MissingPrefixFails)
{
    device_config_t cfg;
    device_config_defaults(&cfg);
    snprintf(cfg.pc_ap_ssid, sizeof(cfg.pc_ap_ssid), "Other_AP");
    char err[128];
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);
}

TEST(DeviceConfigValidate, BadSuffixCharFails)
{
    device_config_t cfg;
    device_config_defaults(&cfg);
    snprintf(cfg.pc_ap_ssid, sizeof(cfg.pc_ap_ssid), "Modu_A!B");
    char err[128];
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);
}

TEST(DeviceConfigValidate, WrongPasswordNoLeak)
{
    device_config_t cfg;
    device_config_defaults(&cfg);
    snprintf(cfg.pc_ap_password, sizeof(cfg.pc_ap_password), "wrong-pass-123");
    char err[128];
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);
    EXPECT_STREQ(err, "热点密码必须使用产品固定值");
    EXPECT_EQ(strstr(err, "wrong-pass-123"), nullptr);
}

TEST(DeviceConfigValidate, IpPortMismatch)
{
    device_config_t cfg;
    char err[128];

    device_config_defaults(&cfg);
    snprintf(cfg.pc_host_ip, sizeof(cfg.pc_host_ip), "10.0.0.1");
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);

    device_config_defaults(&cfg);
    cfg.host_tcp_port = 7000;
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);
}

TEST(DeviceConfigValidate, EmptySsidFails)
{
    device_config_t cfg;
    device_config_defaults(&cfg);
    cfg.pc_ap_ssid[0] = '\0';
    char err[128];
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);
}

TEST(DeviceConfigValidate, OverlongSsidBoundary)
{
    /* 字段容量 33 字节，最大可存 32 字符；32 字符上界可通过校验。
     * 超过 32 的输入在 load 阶段被截断到 32，且校验含 len>32 防御分支。 */
    device_config_t cfg;
    char err[128];
    device_config_defaults(&cfg);
    snprintf(cfg.pc_ap_ssid, sizeof(cfg.pc_ap_ssid),
             "Modu_ABCDEFGHIJKLMNOPQRSTUVWXYZ0"); /* 5 + 27 = 32 */
    EXPECT_EQ(strlen(cfg.pc_ap_ssid), 32u);
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_OK);
}

TEST(DeviceConfigValidate, BackoffBounds)
{
    device_config_t cfg;
    char err[128];

    device_config_defaults(&cfg);
    cfg.wifi_retry_max = 0;
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);

    device_config_defaults(&cfg);
    cfg.wifi_backoff_cap_ms = cfg.wifi_backoff_base_ms - 1;
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);

    device_config_defaults(&cfg);
    cfg.reconnect_backoff_cap_ms = cfg.reconnect_backoff_base_ms - 1;
    EXPECT_EQ(device_config_validate(&cfg, err, sizeof(err)), DEMO_ERR_INVAL);
}
