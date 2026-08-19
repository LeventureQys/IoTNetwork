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
    EXPECT_EQ(p.host_max_conn, 16);
    EXPECT_STREQ(p.target_ssid, "TactileFactory-2.4G");
    EXPECT_EQ(p.device_count, 1);
    EXPECT_EQ(p.duration_s, 0);
    EXPECT_EQ(p.runtime_dir[0], '\0');
    EXPECT_EQ(p.sim_catalog_dir[0], '\0');
    EXPECT_EQ(p.log_dir[0], '\0');
}

TEST(Params, LoadOverrides)
{
    const char *path = "run/test_params_1.json";
    write_temp(path, "{\"host_tcp_port\": 7000, \"heartbeat_interval_ms\": 2000, \"duration_s\": 30}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_OK);
    EXPECT_EQ(p.host_tcp_port, 7000);
    EXPECT_EQ(p.heartbeat_interval_ms, 2000);
    EXPECT_EQ(p.duration_s, 30);
    /* 未覆盖字段保持默认 */
    EXPECT_EQ(p.host_max_conn, 16);
    EXPECT_STREQ(p.target_ssid, "TactileFactory-2.4G");
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
    write_temp(path, "{\"target_ssid\": \"0123456789012345678901234567890123456789\"}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_OK);
    EXPECT_EQ(strlen(p.target_ssid), sizeof(p.target_ssid) - 1);
    remove(path);
}

TEST(Params, StringOverride)
{
    const char *path = "run/test_params_4.json";
    write_temp(path, "{\"target_ssid\": \"Factory-5G\", \"host_virtual_ip\": \"10.0.0.9\"}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_OK);
    EXPECT_STREQ(p.target_ssid, "Factory-5G");
    EXPECT_STREQ(p.host_virtual_ip, "10.0.0.9");
    remove(path);
}

TEST(Params, AdvertiseIpAlias)
{
    const char *path = "run/test_params_5.json";
    write_temp(path, "{\"host_advertise_ip\": \"10.1.2.3\"}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, path), DEMO_OK);
    EXPECT_STREQ(p.host_virtual_ip, "10.1.2.3");
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
    write_temp(cfg_path, "{\"host_tcp_port\": 7100}");
    demo_params_t p;
    EXPECT_EQ(params_load(&p, cfg_path), DEMO_OK);
    EXPECT_EQ(p.host_tcp_port, 7100);
    remove(cfg_path);
}
