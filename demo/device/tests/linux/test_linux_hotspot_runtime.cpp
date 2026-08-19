#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

extern "C" {
#include "linux_hotspot.h"
#include "linux_hotspot_config.h"
#include "linux_hotspot_linux_ops.h"
}

namespace {

struct FakeState {
    std::vector<std::string> commands;
    int fail_command = -1;
    int route_result = 0;
    std::vector<linux_ipv4_route_t> routes;
    bool address_matches = true;
    bool interface_exists = false;
    bool ap_mode = true;
    bool root = true;
    bool binaries_available = true;
    bool pidfile_valid = true;
    bool pid_expected = true;
    int hostapd_pid_failures = 0;
    bool udp_ready = true;
    bool ip_forward = true;
    bool nat_exists = true;
    bool fail_nat_add = false;
    int nat_delete_successes = 0;
    int udp_checks = 0;
    int sleeps = 0;
    uint64_t now = 0;
};

FakeState *g_fake;

std::string JoinArgv(const char *const argv[])
{
    std::string command;
    for (size_t index = 0; argv[index] != nullptr; ++index) {
        if (!command.empty())
            command += ' ';
        command += argv[index];
    }
    return command;
}

int RunArgv(const char *const argv[])
{
    std::string command = JoinArgv(argv);
    g_fake->commands.push_back(command);
    int index = static_cast<int>(g_fake->commands.size()) - 1;
    if (command.find("iptables -t nat -C") != std::string::npos)
        return g_fake->nat_exists ? 0 : 1;
    if (command.find("iptables -t nat -A") != std::string::npos)
        return g_fake->fail_nat_add ? 1 : 0;
    if (command.find("iptables -t nat -D") != std::string::npos) {
        if (g_fake->nat_delete_successes > 0) {
            --g_fake->nat_delete_successes;
            return 0;
        }
        return 1;
    }
    return index == g_fake->fail_command ? 1 : 0;
}

int CollectRoutes(linux_ipv4_route_t *routes, size_t capacity, size_t *count)
{
    if (g_fake->route_result != 0)
        return g_fake->route_result;
    if (g_fake->routes.size() > capacity)
        return -1;
    std::copy(g_fake->routes.begin(), g_fake->routes.end(), routes);
    *count = g_fake->routes.size();
    return 0;
}

int InterfaceHasIpv4(const char *, const linux_ipv4_cidr_t *)
{
    return g_fake->address_matches ? 1 : 0;
}

int PidfileRead(const char *path, pid_t *pid)
{
    if (!g_fake->pidfile_valid)
        return -1;
    if (std::strstr(path, "hostapd") != nullptr &&
        g_fake->hostapd_pid_failures > 0) {
        --g_fake->hostapd_pid_failures;
        return -1;
    }
    *pid = std::strstr(path, "hostapd") != nullptr ? 101 : 202;
    return 0;
}

int PidIsExpected(pid_t, const char *)
{
    return g_fake->pid_expected ? 1 : 0;
}

int PidHasUdpListener(pid_t, uint16_t, uint32_t)
{
    ++g_fake->udp_checks;
    return g_fake->udp_ready ? 1 : 0;
}

uint64_t MonotonicMs()
{
    return g_fake->now;
}

void SleepMs(unsigned int milliseconds)
{
    ++g_fake->sleeps;
    g_fake->now += milliseconds;
}

int InterfaceExists(const char *)
{
    return g_fake->interface_exists ? 1 : 0;
}

int InterfaceIsAp(const char *)
{
    return g_fake->ap_mode ? 1 : 0;
}

int IsRoot()
{
    return g_fake->root ? 1 : 0;
}

int BinaryAvailable(const char *)
{
    return g_fake->binaries_available ? 1 : 0;
}

int StaChannel(const char *)
{
    return 6;
}

int IpForwardEnabled()
{
    return g_fake->ip_forward ? 1 : 0;
}

const linux_hotspot_ops_t kFakeOps = {
    RunArgv,
    CollectRoutes,
    InterfaceHasIpv4,
    PidfileRead,
    PidIsExpected,
    PidHasUdpListener,
    MonotonicMs,
    SleepMs,
    InterfaceExists,
    InterfaceIsAp,
    IsRoot,
    BinaryAvailable,
    StaChannel,
    IpForwardEnabled,
};

class LinuxHotspotRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        work_dir_ = std::filesystem::absolute("run/hotspot_runtime");
        std::filesystem::remove_all(work_dir_);
        std::filesystem::create_directories(work_dir_);
        config_path_ = work_dir_ / "hotspot.json";
        state_ = FakeState{};
        g_fake = &state_;
        WriteConfig(false);
    }

    void TearDown() override
    {
        if (hotspot_ != nullptr) {
            linux_hotspot_destroy(hotspot_);
            hotspot_ = nullptr;
        }
        std::filesystem::remove_all(work_dir_);
        g_fake = nullptr;
    }

    void WriteConfig(bool nat)
    {
        FILE *file = std::fopen(config_path_.string().c_str(), "wb");
        ASSERT_NE(file, nullptr);
        std::string json =
            "{\"enable\":true,\"sta_interface\":\"wlan0\","
            "\"ap_interface\":\"ap0\",\"subnet\":\"10.42.0.1\","
            "\"prefix_length\":24,\"dhcp_start\":\"10.42.0.100\","
            "\"dhcp_end\":\"10.42.0.200\",\"channel\":0,\"nat\":" +
            std::string(nat ? "true" : "false") +
            ",\"hostapd_bin\":\"hostapd\",\"dnsmasq_bin\":\"dnsmasq\","
            "\"work_dir\":\"" + work_dir_.string() + "\"}";
        ASSERT_EQ(std::fwrite(json.data(), 1, json.size(), file), json.size());
        std::fclose(file);
    }

    void Init()
    {
        char error[128] = {0};
        ASSERT_EQ(linux_hotspot_create(&hotspot_, config_path_.string().c_str(),
                                       &kFakeOps, nullptr, error, sizeof(error)),
                  DEMO_OK)
            << error;
        ASSERT_NE(hotspot_, nullptr);
    }

    int FindCommand(const std::string &needle) const
    {
        for (size_t index = 0; index < state_.commands.size(); ++index) {
            if (state_.commands[index].find(needle) != std::string::npos)
                return static_cast<int>(index);
        }
        return -1;
    }

    std::filesystem::path work_dir_;
    std::filesystem::path config_path_;
    FakeState state_;
    linux_hotspot_t *hotspot_ = nullptr;
};

TEST_F(LinuxHotspotRuntimeTest, StartsWithIsolatedDnsmasqAndStopsInReverseOrder)
{
    Init();
    ASSERT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_OK);
    EXPECT_EQ(linux_hotspot_is_active(hotspot_), 1);
    int dnsmasq = FindCommand("dnsmasq --conf-file=/dev/null --port=0");
    ASSERT_GE(dnsmasq, 0);
    EXPECT_NE(state_.commands[dnsmasq].find("--listen-address=10.42.0.1"),
              std::string::npos);
    EXPECT_NE(state_.commands[dnsmasq].find(
                  "--dhcp-range=10.42.0.100,10.42.0.200,255.255.255.0,12h"),
              std::string::npos);
    EXPECT_NE(state_.commands[dnsmasq].find("--dhcp-option=option:router,10.42.0.1"),
              std::string::npos);
    EXPECT_NE(state_.commands[dnsmasq].find("--dhcp-option=54,10.42.0.1"),
              std::string::npos);

    size_t before_stop = state_.commands.size();
    EXPECT_EQ(linux_hotspot_stop(hotspot_), DEMO_OK);
    ASSERT_EQ(state_.commands.size(), before_stop + 4);
    EXPECT_NE(state_.commands[before_stop].find("kill -TERM 202"), std::string::npos);
    EXPECT_NE(state_.commands[before_stop + 1].find("ip addr del 10.42.0.1/24"),
              std::string::npos);
    EXPECT_NE(state_.commands[before_stop + 2].find("kill -TERM 101"), std::string::npos);
    EXPECT_NE(state_.commands[before_stop + 3].find("iw dev ap0 del"), std::string::npos);
    EXPECT_EQ(linux_hotspot_stop(hotspot_), DEMO_OK);
    EXPECT_EQ(state_.commands.size(), before_stop + 4);
}

TEST_F(LinuxHotspotRuntimeTest, RouteCollectionFailureRunsNoCommands)
{
    state_.route_result = -1;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_TRUE(state_.commands.empty());
}

TEST_F(LinuxHotspotRuntimeTest, NonRootCreateFailsBeforeTakingLock)
{
    state_.root = false;
    char error[128] = {0};
    linux_hotspot_t *hotspot = nullptr;
    EXPECT_EQ(linux_hotspot_create(&hotspot, config_path_.string().c_str(),
                                   &kFakeOps, nullptr, error, sizeof(error)),
              DEMO_ERR);
    EXPECT_EQ(hotspot, nullptr);
    EXPECT_FALSE(std::filesystem::exists(work_dir_ / "modu_linux_hotspot.lock"));
    EXPECT_TRUE(state_.commands.empty());
}

TEST_F(LinuxHotspotRuntimeTest, RejectsUnknownInterfaceWithoutDeletingIt)
{
    state_.interface_exists = true;
    Init();
    std::filesystem::path hostapd_pid = work_dir_ / "hostapd_modu.pid";
    std::filesystem::path dnsmasq_pid = work_dir_ / "dnsmasq_modu_ap.pid";
    for (const auto &path : {hostapd_pid, dnsmasq_pid}) {
        FILE *file = std::fopen(path.string().c_str(), "wb");
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(std::fwrite("foreign", 1, 7, file), 7U);
        std::fclose(file);
    }
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_TRUE(state_.commands.empty());
    EXPECT_TRUE(std::filesystem::exists(hostapd_pid));
    EXPECT_TRUE(std::filesystem::exists(dnsmasq_pid));
    EXPECT_EQ(linux_hotspot_stop(hotspot_), DEMO_OK);
    EXPECT_TRUE(std::filesystem::exists(hostapd_pid));
    EXPECT_TRUE(std::filesystem::exists(dnsmasq_pid));
}

TEST_F(LinuxHotspotRuntimeTest, HostapdFailureRollsBackOnlyCreatedInterface)
{
    state_.fail_command = 2;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    ASSERT_EQ(state_.commands.size(), 4U);
    EXPECT_NE(state_.commands.back().find("iw dev ap0 del"), std::string::npos);
    EXPECT_EQ(FindCommand("kill -TERM"), -1);
}

TEST_F(LinuxHotspotRuntimeTest, ApModeFailureRollsBackHostapdAndInterface)
{
    state_.ap_mode = false;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_GE(FindCommand("kill -TERM 101"), 0);
    EXPECT_GE(FindCommand("iw dev ap0 del"), 0);
    EXPECT_EQ(FindCommand("ip addr add"), -1);
}

TEST_F(LinuxHotspotRuntimeTest, WaitsForHostapdPidfileBeforeCheckingApMode)
{
    state_.hostapd_pid_failures = 3;
    Init();
    ASSERT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_OK);
    EXPECT_GE(state_.sleeps, 4);
    EXPECT_EQ(linux_hotspot_is_active(hotspot_), 1);
}

TEST_F(LinuxHotspotRuntimeTest, AddressReadbackFailureRollsBackHostapdAndInterface)
{
    state_.address_matches = false;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_GE(FindCommand("ip addr del 10.42.0.1/24"), 0);
    EXPECT_GE(FindCommand("kill -TERM 101"), 0);
    EXPECT_GE(FindCommand("iw dev ap0 del"), 0);
    EXPECT_EQ(FindCommand("dnsmasq --conf-file=/dev/null"), -1);
}

TEST_F(LinuxHotspotRuntimeTest, DnsmasqReadinessTimeoutChecksTwentyTimes)
{
    state_.udp_ready = false;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_EQ(state_.udp_checks, 20);
    EXPECT_EQ(state_.sleeps, 20);
    EXPECT_GE(FindCommand("kill -TERM 202"), 0);
    EXPECT_EQ(linux_hotspot_is_active(hotspot_), 0);
}

TEST_F(LinuxHotspotRuntimeTest, InvalidPidfileTimesOutWithoutKillingProcesses)
{
    state_.pidfile_valid = false;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_EQ(state_.udp_checks, 0);
    EXPECT_EQ(FindCommand("kill -TERM"), -1);
    EXPECT_GE(FindCommand("iw dev ap0 del"), 0);
}

TEST_F(LinuxHotspotRuntimeTest, DnsmasqCommandFailureRollsBackEarlierStages)
{
    state_.fail_command = 4;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_EQ(FindCommand("kill -TERM 202"), -1);
    EXPECT_GE(FindCommand("ip addr del 10.42.0.1/24"), 0);
    EXPECT_GE(FindCommand("kill -TERM 101"), 0);
}

TEST_F(LinuxHotspotRuntimeTest, WrongPidIdentityIsNeverKilled)
{
    state_.pid_expected = false;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_EQ(FindCommand("kill -TERM"), -1);
    EXPECT_GE(FindCommand("iw dev ap0 del"), 0);
}

TEST_F(LinuxHotspotRuntimeTest, NatRequiresForwardingAndUsesExactOwnedRule)
{
    WriteConfig(true);
    state_.ip_forward = false;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_EQ(FindCommand("iptables"), -1);

    if (hotspot_ != nullptr) {
        linux_hotspot_destroy(hotspot_);
        hotspot_ = nullptr;
    }
    state_.commands.clear();
    state_.ip_forward = true;
    Init();
    ASSERT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_OK);
    int check = FindCommand("iptables -t nat -C POSTROUTING -s 10.42.0.0/24");
    ASSERT_GE(check, 0);
    EXPECT_NE(state_.commands[check].find("-o wlan0 -m comment --comment modu-provision-nat"),
              std::string::npos);
    EXPECT_EQ(FindCommand("-A POSTROUTING"), -1);
    linux_hotspot_stop(hotspot_);
    EXPECT_EQ(FindCommand("-D POSTROUTING"), -1);
}

TEST_F(LinuxHotspotRuntimeTest, CreatedNatRuleIsDeletedUntilAbsent)
{
    WriteConfig(true);
    state_.nat_exists = false;
    state_.nat_delete_successes = 1;
    Init();
    ASSERT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_OK);
    EXPECT_GE(FindCommand("iptables -t nat -A POSTROUTING"), 0);
    linux_hotspot_stop(hotspot_);
    int first_delete = FindCommand("iptables -t nat -D POSTROUTING");
    ASSERT_GE(first_delete, 0);
    ASSERT_LT(static_cast<size_t>(first_delete + 1), state_.commands.size());
    EXPECT_NE(state_.commands[first_delete + 1].find("iptables -t nat -D POSTROUTING"),
              std::string::npos);
}

TEST_F(LinuxHotspotRuntimeTest, NatAddFailureRollsBackHotspot)
{
    WriteConfig(true);
    state_.nat_exists = false;
    state_.fail_nat_add = true;
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_GE(FindCommand("iptables -t nat -A POSTROUTING"), 0);
    EXPECT_GE(FindCommand("kill -TERM 202"), 0);
    EXPECT_GE(FindCommand("iw dev ap0 del"), 0);
    EXPECT_EQ(linux_hotspot_is_active(hotspot_), 0);
}

TEST_F(LinuxHotspotRuntimeTest, ExistingWorkFileIsNotOverwrittenOrRemoved)
{
    Init();
    std::filesystem::path config = work_dir_ / "hostapd_modu.conf";
    FILE *file = std::fopen(config.string().c_str(), "wb");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(std::fwrite("foreign", 1, 7, file), 7U);
    std::fclose(file);
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_TRUE(state_.commands.empty());
    EXPECT_TRUE(std::filesystem::exists(config));
}

TEST_F(LinuxHotspotRuntimeTest, CreateFailsWhenAnotherOwnerHoldsLock)
{
    std::filesystem::path lock_path = work_dir_ / "modu_linux_hotspot.lock";
    int lock = open(lock_path.string().c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(lock, 0);
    ASSERT_EQ(flock(lock, LOCK_EX | LOCK_NB), 0);
    char error[128] = {0};
    linux_hotspot_t *hotspot = nullptr;
    EXPECT_EQ(linux_hotspot_create(&hotspot, config_path_.string().c_str(),
                                   &kFakeOps, nullptr, error, sizeof(error)),
              DEMO_ERR);
    EXPECT_EQ(hotspot, nullptr);
    flock(lock, LOCK_UN);
    close(lock);
}

TEST_F(LinuxHotspotRuntimeTest, SecondInstanceFailsWhileFirstHoldsLock)
{
    Init();
    char error[128] = {0};
    linux_hotspot_t *second = nullptr;
    EXPECT_EQ(linux_hotspot_create(&second, config_path_.string().c_str(),
                                   &kFakeOps, nullptr, error, sizeof(error)),
              DEMO_ERR);
    EXPECT_EQ(second, nullptr);
    /* 销毁第一个实例后锁释放，可以再次创建（部分初始化销毁的逆序清理） */
    linux_hotspot_destroy(hotspot_);
    hotspot_ = nullptr;
    ASSERT_EQ(linux_hotspot_create(&hotspot_, config_path_.string().c_str(),
                                   &kFakeOps, nullptr, error, sizeof(error)),
              DEMO_OK);
}

TEST_F(LinuxHotspotRuntimeTest, DestroyAfterStartCleansUpAndReleasesLock)
{
    Init();
    ASSERT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_OK);
    linux_hotspot_destroy(hotspot_);
    hotspot_ = nullptr;
    EXPECT_GE(FindCommand("iw dev ap0 del"), 0);
    /* 锁文件本身保留（与旧实现一致），但 flock 已释放：验证可再次创建 */
    char error[128] = {0};
    linux_hotspot_t *again = nullptr;
    ASSERT_EQ(linux_hotspot_create(&again, config_path_.string().c_str(),
                                   &kFakeOps, nullptr, error, sizeof(error)),
              DEMO_OK);
    linux_hotspot_destroy(again);
}

TEST_F(LinuxHotspotRuntimeTest, RejectsControlCharactersInCredentials)
{
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu\nTest", "password123"),
              DEMO_ERR_INVAL);
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "pass\rword123"),
              DEMO_ERR_INVAL);
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "short"),
              DEMO_ERR_INVAL);
    EXPECT_TRUE(state_.commands.empty());
}

TEST_F(LinuxHotspotRuntimeTest, PassesSpecialCharactersAsLiteralArgvElements)
{
    Init();
    /* 单引号/双引号/反斜杠/$ 等字符在 argv 调用下必须原样传递，
     * 不做 shell 转义或解释（命令记录为按空格连接的 argv）。 */
    const char *ssid = "Modu'Quote\"Back$lash";
    const char *password = "p@ss\"w'ord\\$x$(id)";
    ASSERT_EQ(linux_hotspot_start(hotspot_, ssid, password), DEMO_OK);
    int create = FindCommand("iw dev wlan0 interface add");
    ASSERT_GE(create, 0);
    std::ifstream config(work_dir_ / "hostapd_modu.conf");
    ASSERT_TRUE(config.is_open());
    std::string contents((std::istreambuf_iterator<char>(config)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find(std::string("ssid=") + ssid + "\n"), std::string::npos)
        << "SSID 必须原样写入 hostapd 配置";
    EXPECT_NE(contents.find(std::string("wpa_passphrase=") + password + "\n"),
              std::string::npos)
        << "密码必须原样写入 hostapd 配置";
    int hostapd = FindCommand("hostapd -B");
    ASSERT_GE(hostapd, 0);
    /* hostapd 参数不含 ssid；配置写入由 write_hostapd_config 完成，此处仅
     * 验证命令本身不含 shell 元字符处理痕迹 */
    EXPECT_EQ(FindCommand("sh -c"), -1);
    EXPECT_EQ(FindCommand("bash -c"), -1);
}

TEST_F(LinuxHotspotRuntimeTest, ConflictingRoutesFailBeforeResourceCreation)
{
    for (const auto &entry : std::vector<std::pair<const char *, int>>{
             {"10.42.0.0", 24}, {"10.43.0.0", 24},
             {"172.31.250.0", 24}, {"192.168.250.0", 24}}) {
        linux_ipv4_route_t route{};
        ASSERT_EQ(linux_ipv4_cidr_parse(entry.first, entry.second,
                                        &route.destination), DEMO_OK);
        state_.routes.push_back(route);
    }
    Init();
    EXPECT_EQ(linux_hotspot_start(hotspot_, "Modu_Test", "password123"), DEMO_ERR);
    EXPECT_TRUE(state_.commands.empty());
}

}  // namespace
