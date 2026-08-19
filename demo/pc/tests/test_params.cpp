#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "params.h"

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

static void write_temp(const char *path, const char *content)
{
    std::filesystem::create_directories("run");
    FILE *f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fputs(content, f);
    fclose(f);
}

TEST(Params, Defaults)
{
    demo_params_t p;
    EXPECT_EQ(params_load(&p, nullptr), DEMO_OK);
    EXPECT_EQ(p.host_tcp_port, 5935);
    EXPECT_EQ(p.heartbeat_interval_ms, 10000);
    EXPECT_EQ(p.heartbeat_dead_ms, 0);
    EXPECT_EQ(p.hello_timeout_ms, 5000);
    EXPECT_STREQ(p.pc_ap_ssid, "Modu_PC");
    EXPECT_STREQ(p.pc_ap_password, "modu_leventure");
    EXPECT_STREQ(p.pc_ap_ip, "192.168.137.1");
    EXPECT_EQ(p.pc_ap_prefix_length, 24);
    EXPECT_EQ(p.duration_s, 0);
    EXPECT_EQ(p.runtime_dir[0], '\0');
    EXPECT_EQ(p.sim_catalog_dir[0], '\0');
    EXPECT_EQ(p.log_dir[0], '\0');
}

TEST(Params, LoadOverrides)
{
    const char *path = "run/test_params_1.json";
    write_temp(path, "{\"host_tcp_port\": 5935, \"heartbeat_interval_ms\": 2000, \"duration_s\": 30, "
                     "\"pc_ap_ssid\": \"Modu_Test\"}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_OK);
    EXPECT_EQ(p.host_tcp_port, 5935);
    EXPECT_EQ(p.heartbeat_interval_ms, 2000);
    EXPECT_EQ(p.duration_s, 30);
    EXPECT_STREQ(p.pc_ap_ssid, "Modu_Test");
    /* 未覆盖字段保持默认 */
    EXPECT_EQ(p.pc_ap_prefix_length, 24);
    EXPECT_STREQ(p.pc_ap_ip, "192.168.137.1");
    remove(path);
}

TEST(Params, MissingFileKeepsDefaults)
{
    demo_params_t p;
    EXPECT_EQ(params_load(&p, "run/not_exist_xyz.json"), DEMO_OK);
    EXPECT_EQ(p.host_tcp_port, 5935);
}

TEST(Params, InvalidJson)
{
    const char *path = "run/test_params_2.json";
    write_temp(path, "{invalid json");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_ERR);
    remove(path);
}

TEST(Params, LongStringTruncated)
{
    const char *path = "run/test_params_3.json";
    write_temp(path, "{\"pc_ap_ssid\": \"Modu_0123456789012345678901234567890123456789\"}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_OK);
    EXPECT_EQ(strlen(p.pc_ap_ssid), sizeof(p.pc_ap_ssid) - 1);
    remove(path);
}

TEST(Params, StringOverride)
{
    const char *path = "run/test_params_4.json";
    write_temp(path, "{\"pc_ap_ssid\": \"Modu_Factory\", \"pc_ap_ip\": \"192.168.137.1\"}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_OK);
    EXPECT_STREQ(p.pc_ap_ssid, "Modu_Factory");
    EXPECT_STREQ(p.pc_ap_ip, "192.168.137.1");
    remove(path);
}

TEST(Params, PcPathFields)
{
    const char *path = "run/test_params_6.json";
    write_temp(path,
               "{\"runtime_dir\": \"var/run\", \"sim_catalog_dir\": \"var/catalog\", "
               "\"log_dir\": \"var/log\"}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_OK);
    EXPECT_STREQ(p.runtime_dir, "var/run");
    EXPECT_STREQ(p.sim_catalog_dir, "var/catalog");
    EXPECT_STREQ(p.log_dir, "var/log");
    remove(path);
}

TEST(Params, NotDependentOnCwd)
{
    /* 配置不依赖 CWD：用绝对路径在任意 CWD 下加载 */
    char abs_path[520];
    ASSERT_NE(getcwd(abs_path, sizeof(abs_path)), nullptr);
    char cfg_path[1040];
    snprintf(cfg_path, sizeof(cfg_path), "%s/run/test_params_abs.json", abs_path);
    write_temp(cfg_path, "{\"host_tcp_port\": 5935}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, cfg_path), DEMO_OK);
    EXPECT_EQ(p.host_tcp_port, 5935);
    remove(cfg_path);
}

/* ---------- params_validate 契约用例 ---------- */

TEST(ParamsValidate, DefaultsOk)
{
    demo_params_t p;
    params_defaults(&p);
    char err[128];
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_OK);
}

TEST(ParamsValidate, ModuPcOk)
{
    demo_params_t p;
    params_defaults(&p);
    snprintf(p.pc_ap_ssid, sizeof(p.pc_ap_ssid), "Modu_PC");
    char err[128];
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_OK);
}

TEST(ParamsValidate, NullArgs)
{
    demo_params_t p;
    params_defaults(&p);
    char err[128];
    EXPECT_EQ(params_validate(nullptr, err, sizeof(err)), DEMO_ERR_INVAL);
    EXPECT_EQ(params_validate(&p, nullptr, sizeof(err)), DEMO_ERR_INVAL);
    EXPECT_EQ(params_validate(&p, err, 0), DEMO_ERR_INVAL);
}

TEST(ParamsValidate, MissingPrefixFails)
{
    demo_params_t p;
    params_defaults(&p);
    snprintf(p.pc_ap_ssid, sizeof(p.pc_ap_ssid), "Other_AP");
    char err[128];
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_ERR_INVAL);
}

TEST(ParamsValidate, BadSuffixCharFails)
{
    demo_params_t p;
    params_defaults(&p);
    snprintf(p.pc_ap_ssid, sizeof(p.pc_ap_ssid), "Modu_A!B");
    char err[128];
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_ERR_INVAL);
}

TEST(ParamsValidate, WrongPasswordNoLeak)
{
    demo_params_t p;
    params_defaults(&p);
    snprintf(p.pc_ap_password, sizeof(p.pc_ap_password), "wrong-pass-123");
    char err[128];
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_ERR_INVAL);
    EXPECT_STREQ(err, "热点密码必须使用产品固定值");
    EXPECT_EQ(strstr(err, "wrong-pass-123"), nullptr);
}

TEST(ParamsValidate, IpPrefixPortMismatch)
{
    demo_params_t p;
    char err[128];

    params_defaults(&p);
    snprintf(p.pc_ap_ip, sizeof(p.pc_ap_ip), "10.0.0.1");
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_ERR_INVAL);

    params_defaults(&p);
    p.pc_ap_prefix_length = 16;
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_ERR_INVAL);

    params_defaults(&p);
    p.host_tcp_port = 7000;
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_ERR_INVAL);
}

TEST(ParamsValidate, EmptySsidFails)
{
    demo_params_t p;
    params_defaults(&p);
    p.pc_ap_ssid[0] = '\0';
    char err[128];
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_ERR_INVAL);
}

TEST(ParamsValidate, OverlongSsidBoundary)
{
    /* 字段容量 33 字节，最大可存 32 字符；32 字符上界可通过校验。
     * 超过 32 的输入在 load 阶段被截断到 32，且校验含 len>32 防御分支，
     * 因此超长 SSID 无法形成合法配置。 */
    demo_params_t p;
    char err[128];
    params_defaults(&p);
    snprintf(p.pc_ap_ssid, sizeof(p.pc_ap_ssid),
             "Modu_ABCDEFGHIJKLMNOPQRSTUVWXYZ0"); /* 5 + 27 = 32 */
    EXPECT_EQ(strlen(p.pc_ap_ssid), 32u);
    EXPECT_EQ(params_validate(&p, err, sizeof(err)), DEMO_OK);
}
