#include "test_host_fixture.h"

#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

struct PathTest : ::testing::Test {
    std::string dir;
    void SetUp() override
    {
        fake_backend_reset();
        dir = TmpDir("paths");
        CleanDir(dir);
    }
    void TearDown() override { CleanDir(dir); }
};

static std::string GetCwd()
{
    char buf[1024];
#ifdef _WIN32
    GetCurrentDirectoryA((DWORD)sizeof(buf), buf);
#else
    if (getcwd(buf, sizeof(buf)) == nullptr)
        buf[0] = '\0';
#endif
    return std::string(buf);
}

} // namespace

TEST_F(PathTest, CwdNeverChanged)
{
    WriteConfig(dir, "{\"power_on_jitter_max_ms\":50}");
    std::string before = GetCwd();
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
    EXPECT_EQ(GetCwd(), before);
}

TEST_F(PathTest, FreshDeletesOnlyOwnNvs)
{
    std::error_code ec;
    std::filesystem::create_directories(dir + "/run", ec);
    WriteFile(dir + "/run/dev0.nvs.json", "MARKER_OLD_DEV0");
    WriteFile(dir + "/run/dev1.nvs.json", "MARKER_DEV1");
    WriteFile(dir + "/run/keep.txt", "keep");

    WriteConfig(dir, "{\"power_on_jitter_max_ms\":50}");
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.fresh = 1; /* 只删除本设备（index 0）的 NVS */
    device_host_t *h = CreateHost(o);
    DestroyHost(&h);

    /* fresh 删除了旧 dev0 文件（运行中重建的文件不应包含旧标记） */
    ASSERT_TRUE(std::filesystem::exists(dir + "/run/dev0.nvs.json"));
    std::string dev0 = [&]() {
        std::ifstream in(dir + "/run/dev0.nvs.json");
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }();
    EXPECT_EQ(dev0.find("MARKER_OLD_DEV0"), std::string::npos);
    /* 其他设备文件与无关文件不受影响 */
    std::string dev1 = [&]() {
        std::ifstream in(dir + "/run/dev1.nvs.json");
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }();
    EXPECT_NE(dev1.find("MARKER_DEV1"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(dir + "/run/keep.txt"));
}

TEST_F(PathTest, FreshOffKeepsNvs)
{
    std::error_code ec;
    std::filesystem::create_directories(dir + "/run", ec);
    WriteFile(dir + "/run/dev0.nvs.json", "{\"k\":\"v\"}");

    WriteConfig(dir, "{\"power_on_jitter_max_ms\":50}");
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    DestroyHost(&h);
    EXPECT_TRUE(std::filesystem::exists(dir + "/run/dev0.nvs.json"));
}

TEST_F(PathTest, ConfigRelativeNvsDirResolved)
{
    /* nvs_dir 相对 → 按配置文件目录解析，且创建目录 */
    WriteConfig(dir, "{\"power_on_jitter_max_ms\":50,\"nvs_dir\":\"sub/run\"}");
    std::string cfg_path;
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    DestroyHost(&h);
    EXPECT_TRUE(std::filesystem::exists(dir + "/sub/run"));
}

TEST_F(PathTest, RuntimeDirOverride)
{
    std::error_code ec;
    std::filesystem::create_directories(dir + "/override", ec);
    WriteConfig(dir, "{\"power_on_jitter_max_ms\":50}");
    std::string cfg_path;
    std::string override_dir = dir + "/override";
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    o.runtime_dir = override_dir.c_str();
    device_host_t *h = CreateHost(o);
    DestroyHost(&h);
    /* --runtime-dir 覆盖配置 nvs_dir：NVS 落在 override，不在默认 run */
    EXPECT_TRUE(std::filesystem::exists(dir + "/override/dev0.nvs.json"));
    EXPECT_FALSE(std::filesystem::exists(dir + "/run"));
}

TEST_F(PathTest, MissingConfigUsesDefaults)
{
    /* config 指向不存在文件 → 内置默认 + 警告，仍可创建 */
    std::string cfg_path = dir + "/nope.json";
    device_host_options_t o = MakeOptions(dir, &cfg_path);
    device_host_t *h = CreateHost(o);
    device_error_t e;
    ASSERT_EQ(device_host_start(h, &e), DEVICE_OK);
    EXPECT_EQ(device_host_request_stop(h), DEVICE_OK);
    EXPECT_EQ(device_host_join(h, 5000, &e), DEVICE_OK);
    DestroyHost(&h);
}
