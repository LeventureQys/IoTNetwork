#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

extern "C" {
#include "device_linux_backend.h"
#include "linux_nvs.h"
#include "linux_wifi.h"
}

namespace {

/* ---------------- wifi fake ops（argv 直传断言） ---------------- */

struct WifiFakeState {
    std::vector<std::vector<std::string>> argv_calls;
    std::string output; /* 按命令前缀匹配的固定输出 */
    std::string command_contains; /* 输出匹配条件：命令连接串包含该子串 */
    int exit_code = 0;
};

WifiFakeState *g_wifi_fake;

int WExecArgv(const char *const argv[], char *output, size_t output_capacity)
{
    std::vector<std::string> call;
    std::string joined;
    for (size_t index = 0; argv[index] != nullptr; ++index) {
        call.push_back(argv[index]);
        if (!joined.empty())
            joined += ' ';
        joined += argv[index];
    }
    g_wifi_fake->argv_calls.push_back(call);
    if (output != NULL && output_capacity > 0) {
        std::string text;
        if (g_wifi_fake->command_contains.empty() ||
            joined.find(g_wifi_fake->command_contains) != std::string::npos)
            text = g_wifi_fake->output;
        snprintf(output, output_capacity, "%s", text.c_str());
    }
    return g_wifi_fake->exit_code;
}

const linux_wifi_ops_t kWifiFakeOps = {WExecArgv};

/* ---------------- 测试夹具 ---------------- */

class LinuxBackendTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        work_dir_ = std::filesystem::absolute("run/linux_backend");
        std::filesystem::remove_all(work_dir_);
        std::filesystem::create_directories(work_dir_);
        device_config_path_ = work_dir_ / "device_linux.json";
        nvs_file_ = work_dir_ / "device.nvs.json";
        device_config_path_str_ = device_config_path_.string();
        nvs_file_str_ = nvs_file_.string();
        wifi_fake_ = WifiFakeState{};
        g_wifi_fake = &wifi_fake_;
    }

    void TearDown() override
    {
        g_wifi_fake = nullptr;
        std::filesystem::remove_all(work_dir_);
    }

    void WriteDeviceConfig(const std::string &extra_fields)
    {
        std::ofstream file(device_config_path_);
        ASSERT_TRUE(file.is_open());
        file << "{"
             << (extra_fields.empty() ? "\"enable\":true," : extra_fields + ",")
             << "\"sta_interface\":\"wlan0\","
                "\"nvs_file\":\"" +
                     nvs_file_.string() + "\"}";
        file.close();
    }

    device_linux_backend_options_t Options()
    {
        device_linux_backend_options_t options{};
        options.config_path = device_config_path_str_.c_str();
        options.nvs_file = nvs_file_str_.c_str();
        options.sta_interface = nullptr;
        options.device_index = 1;
        return options;
    }

    std::filesystem::path work_dir_;
    std::filesystem::path device_config_path_;
    std::filesystem::path nvs_file_;
    std::string device_config_path_str_;
    std::string nvs_file_str_;
    WifiFakeState wifi_fake_;
};

/* ---------------- 后端 create（beta v1.1：无热点依赖，普通用户可创建） ---------------- */

TEST_F(LinuxBackendTest, CreateRejectsInvalidDeviceIndex)
{
    device_linux_backend_options_t options = Options();
    options.device_index = 16;
    device_backend_instance_t instance;
    device_error_t error;
    memset(&instance, 0xAA, sizeof(instance));
    EXPECT_EQ(device_linux_backend_create(&options, &instance, &error),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(instance.vtable, nullptr);
    EXPECT_EQ(instance.user, nullptr);
    EXPECT_EQ(instance.destroy_user, nullptr);
    EXPECT_EQ(error.code, DEVICE_ERR_INVALID_ARGUMENT);
}

TEST_F(LinuxBackendTest, CreateFailsClosedWhenBackendDisabled)
{
    WriteDeviceConfig("\"enable\":false");
    device_linux_backend_options_t options = Options();
    device_backend_instance_t instance;
    device_error_t error;
    memset(&instance, 0xAA, sizeof(instance));
    EXPECT_EQ(device_linux_backend_create(&options, &instance, &error),
              DEVICE_ERR_BACKEND_UNAVAILABLE);
    EXPECT_EQ(instance.vtable, nullptr);
    EXPECT_EQ(instance.user, nullptr);
    EXPECT_EQ(instance.destroy_user, nullptr);
}

TEST_F(LinuxBackendTest, CreateFailsClosedWhenNvsUnwritable)
{
    device_linux_backend_options_t options = Options();
    options.nvs_file = "/nonexistent-dir/device.nvs.json";
    device_backend_instance_t instance;
    device_error_t error;
    memset(&instance, 0xAA, sizeof(instance));
    EXPECT_NE(device_linux_backend_create(&options, &instance, &error), DEVICE_OK);
    EXPECT_EQ(instance.vtable, nullptr);
    EXPECT_EQ(instance.user, nullptr);
    EXPECT_EQ(instance.destroy_user, nullptr);
}

/* beta v1.1 关键回归：普通用户（非 root）且无热点依赖下，纯 STA 后端可创建。 */
TEST_F(LinuxBackendTest, CreateStaBackendSucceedsWithoutRoot)
{
    WriteDeviceConfig("");
    device_linux_backend_options_t options = Options();
    device_backend_instance_t instance;
    device_error_t error;
    memset(&instance, 0xAA, sizeof(instance));
    ASSERT_EQ(device_linux_backend_create(&options, &instance, &error), DEVICE_OK)
        << error.message;
    EXPECT_NE(instance.vtable, nullptr);
    EXPECT_NE(instance.user, nullptr);
    EXPECT_NE(instance.destroy_user, nullptr);
    instance.destroy_user(instance.user);
    memset(&instance, 0, sizeof(instance));

    /* 销毁后可再次创建（逆序清理无残留） */
    ASSERT_EQ(device_linux_backend_create(&options, &instance, &error), DEVICE_OK)
        << error.message;
    instance.destroy_user(instance.user);
}

/* ---------------- wifi 组件：argv 直传与解析（新签名，直接指定 STA 接口） ---------------- */

static linux_wifi_t *CreateWifi()
{
    linux_wifi_t *wifi = nullptr;
    EXPECT_EQ(linux_wifi_create(&wifi, "wlan0", &kWifiFakeOps), DEMO_OK);
    return wifi;
}

TEST_F(LinuxBackendTest, WifiCreateDefaultsInterfaceWhenNull)
{
    linux_wifi_t *wifi = nullptr;
    ASSERT_EQ(linux_wifi_create(&wifi, nullptr, &kWifiFakeOps), DEMO_OK);
    EXPECT_STREQ(linux_wifi_sta_interface(wifi), "wlP2p33s0");
    linux_wifi_destroy(wifi);
}

TEST_F(LinuxBackendTest, WifiConnectPassesCredentialsAsLiteralArgv)
{
    linux_wifi_t *wifi = CreateWifi();
    ASSERT_NE(wifi, nullptr);

    const char *ssid = "Tactile 'Quoted' \\ $x";
    const char *password = "p@ss word ' \" \\ $((1))";
    wifi_fake_.exit_code = 1; /* 失败路径，不等待 DHCP */
    wifi_fake_.output = "Error: No network with SSID 'x' found.";

    wifi_reason_t reason = WIFI_REASON_OK;
    EXPECT_EQ(linux_wifi_connect(wifi, ssid, password, &reason), DEMO_ERR);
    EXPECT_EQ(reason, WIFI_REASON_NO_AP_FOUND);
    ASSERT_FALSE(wifi_fake_.argv_calls.empty());

    const std::vector<std::string> &call = wifi_fake_.argv_calls.front();
    ASSERT_GE(call.size(), 7U);
    EXPECT_EQ(call[0], "nmcli");
    EXPECT_EQ(call[3], "connect");
    EXPECT_EQ(call[4], ssid);      /* 原样，不做 shell 转义 */
    EXPECT_EQ(call[5], "password");
    EXPECT_EQ(call[6], password);  /* 原样，不做 shell 转义 */

    linux_wifi_destroy(wifi);
}

TEST_F(LinuxBackendTest, WifiConnectMapsAuthFailureReason)
{
    linux_wifi_t *wifi = CreateWifi();
    ASSERT_NE(wifi, nullptr);

    wifi_fake_.exit_code = 1;
    wifi_fake_.output = "Error: Connection activation failed: Secrets were required.";
    wifi_reason_t reason = WIFI_REASON_OK;
    EXPECT_EQ(linux_wifi_connect(wifi, "AP", "wrongpass", &reason), DEMO_ERR);
    EXPECT_EQ(reason, WIFI_REASON_AUTH_FAIL);

    linux_wifi_destroy(wifi);
}

TEST_F(LinuxBackendTest, WifiGetGatewayParsesViaLine)
{
    linux_wifi_t *wifi = CreateWifi();
    ASSERT_NE(wifi, nullptr);

    wifi_fake_.output = "default via 192.168.1.1 dev wlan0 proto dhcp metric 600";
    uint32_t ip = 0;
    EXPECT_EQ(linux_wifi_get_gateway(wifi, &ip), DEMO_OK);
    /* 网络字节序表示：192.168.1.1 -> 0x0101A8C0（小端存储） */
    EXPECT_EQ(ip, 0x0101A8C0U);

    linux_wifi_destroy(wifi);
}

TEST_F(LinuxBackendTest, WifiGetIpParsesInetLine)
{
    linux_wifi_t *wifi = CreateWifi();
    ASSERT_NE(wifi, nullptr);

    wifi_fake_.output =
        "2: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500\n"
        "    inet 10.0.0.5/24 brd 10.0.0.255 scope global dynamic noprefixroute wlan0";
    uint32_t ip = 0;
    EXPECT_EQ(linux_wifi_get_ip(wifi, &ip), DEMO_OK);
    EXPECT_EQ(ip, 0x0500000AU); /* 10.0.0.5 -> 网络字节序小端 0x0500000A */

    linux_wifi_destroy(wifi);
}

TEST_F(LinuxBackendTest, WifiScanParsesNmcliLines)
{
    linux_wifi_t *wifi = CreateWifi();
    ASSERT_NE(wifi, nullptr);

    wifi_fake_.output =
        "Modu_0001:-45:WPA2\n"
        "TactileFactory-2.4G:-60:WPA1 WPA2\n"
        "\n";
    net_ap_info_t aps[4];
    int count = 4;
    EXPECT_EQ(linux_wifi_scan(wifi, aps, &count), DEMO_OK);
    EXPECT_EQ(count, 2);
    EXPECT_STREQ(aps[0].ssid, "Modu_0001");
    EXPECT_EQ(aps[0].rssi, -45);
    EXPECT_EQ(aps[0].band_2g, 1);
    EXPECT_STREQ(aps[1].ssid, "TactileFactory-2.4G");
    EXPECT_EQ(aps[1].rssi, -60);

    linux_wifi_destroy(wifi);
}

/* ---------------- NVS 组件：文件持久化 ---------------- */

TEST_F(LinuxBackendTest, NvsRoundTripAndErase)
{
    linux_nvs_t *nvs = nullptr;
    ASSERT_EQ(linux_nvs_create(&nvs, nvs_file_.string().c_str()), DEMO_OK);
    ASSERT_NE(nvs, nullptr);

    const uint8_t blob[] = {'a', 0x00, 0xFF, '"', '\\'};
    ASSERT_EQ(linux_nvs_set(nvs, "wifi_creds", blob, sizeof(blob)), DEMO_OK);

    uint8_t buffer[16];
    int length = (int)sizeof(buffer);
    EXPECT_EQ(linux_nvs_get(nvs, "wifi_creds", buffer, &length), DEMO_OK);
    EXPECT_EQ(length, (int)sizeof(blob));
    EXPECT_EQ(std::memcmp(buffer, blob, sizeof(blob)), 0);

    length = (int)sizeof(buffer);
    EXPECT_EQ(linux_nvs_get(nvs, "missing", buffer, &length), DEMO_ERR);
    EXPECT_EQ(length, 0);

    EXPECT_EQ(linux_nvs_erase(nvs, "wifi_creds"), DEMO_OK);
    length = (int)sizeof(buffer);
    EXPECT_EQ(linux_nvs_get(nvs, "wifi_creds", buffer, &length), DEMO_ERR);
    EXPECT_EQ(length, 0);

    linux_nvs_destroy(nvs);
}

/* 链接/编译守卫：本测试二进制不引用任何 sim 符号；若误引入会链接失败。 */
TEST_F(LinuxBackendTest, NoSimFallbackGuaranteedByContract)
{
    device_linux_backend_options_t options = Options();
    options.nvs_file = "/nonexistent/hotspot.json";
    device_backend_instance_t instance;
    device_error_t error;
    device_result_t result =
        device_linux_backend_create(&options, &instance, &error);
    EXPECT_NE(result, DEVICE_OK);
    EXPECT_EQ(instance.vtable, nullptr);
    EXPECT_EQ(instance.user, nullptr);
    EXPECT_EQ(instance.destroy_user, nullptr);
}

}  // namespace
