#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include "linux_hotspot_config.h"
}

namespace {

const char *kTempConfig = "run/test_linux_hotspot_config.json";

void WriteConfig(const std::string &content)
{
    std::filesystem::create_directories("run");
    FILE *file = fopen(kTempConfig, "wb");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(fwrite(content.data(), 1, content.size(), file), content.size());
    fclose(file);
}

linux_hotspot_cfg_t Defaults()
{
    linux_hotspot_cfg_t config;
    linux_hotspot_cfg_defaults(&config);
    return config;
}

uint32_t Ip(const char *address)
{
    linux_ipv4_cidr_t cidr;
    EXPECT_EQ(linux_ipv4_cidr_parse(address, 32, &cidr), DEMO_OK);
    return cidr.address;
}

linux_ipv4_route_t Route(const char *address, int prefix)
{
    linux_ipv4_route_t route{};
    EXPECT_EQ(linux_ipv4_cidr_parse(address, prefix, &route.destination), DEMO_OK);
    return route;
}

class LinuxHotspotConfigTest : public ::testing::Test {
protected:
    void TearDown() override { std::remove(kTempConfig); }
};

TEST_F(LinuxHotspotConfigTest, DefaultsMatchDocumentedConfiguration)
{
    linux_hotspot_cfg_t config = Defaults();
    EXPECT_EQ(config.enable, 1);
    EXPECT_STREQ(config.subnet, "10.42.0.1");
    EXPECT_EQ(config.prefix_length, 24);
    EXPECT_STREQ(config.dhcp_start, "10.42.0.100");
    EXPECT_STREQ(config.dhcp_end, "10.42.0.200");
    EXPECT_EQ(config.nat, 0);
}

TEST_F(LinuxHotspotConfigTest, LoadsDefaultsAndOverrides)
{
    WriteConfig(R"({"prefix_length":30,"subnet":"10.0.0.1","dhcp_start":"10.0.0.2","dhcp_end":"10.0.0.2"})");
    linux_hotspot_cfg_t config;
    char error[128];
    ASSERT_EQ(linux_hotspot_cfg_load(&config, kTempConfig, error, sizeof(error)), DEMO_OK) << error;
    EXPECT_EQ(config.prefix_length, 30);
    EXPECT_STREQ(config.sta_interface, "wlP2p33s0");
    EXPECT_STREQ(config.dhcp_start, "10.0.0.2");
}

TEST_F(LinuxHotspotConfigTest, RepositoryJsonMatchesDefaults)
{
    linux_hotspot_cfg_t expected = Defaults();
    linux_hotspot_cfg_t actual;
    char error[128];
    /* 仓库配置位于 demo/device/config/linux_hotspot.json：
     * 本文件（tests/linux/..）向上三级为 demo/device。 */
    const std::filesystem::path repository_config =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
        "config/linux_hotspot.json";
    ASSERT_EQ(linux_hotspot_cfg_load(&actual, repository_config.string().c_str(), error,
                                     sizeof(error)), DEMO_OK) << error;
    EXPECT_EQ(std::memcmp(&actual, &expected, sizeof(actual)), 0);
}

TEST_F(LinuxHotspotConfigTest, ParsesSupportedCidrsAndFormatsHostOrder)
{
    linux_ipv4_cidr_t cidr;
    char formatted[16];
    for (int prefix : {16, 24, 30}) {
        ASSERT_EQ(linux_ipv4_cidr_parse("10.42.3.9", prefix, &cidr), DEMO_OK);
        EXPECT_EQ(cidr.prefix_length, prefix);
    }
    ASSERT_EQ(linux_ipv4_cidr_parse("10.42.3.9", 24, &cidr), DEMO_OK);
    linux_ipv4_format(cidr.network, formatted, sizeof(formatted));
    EXPECT_STREQ(formatted, "10.42.3.0");
    linux_netmask_format(cidr.netmask, formatted, sizeof(formatted));
    EXPECT_STREQ(formatted, "255.255.255.0");
    EXPECT_EQ(linux_ipv4_cidr_parse("10.0.0.1", 33, &cidr), DEMO_ERR_INVAL);
    EXPECT_EQ(cidr.address, 0U);
}

TEST_F(LinuxHotspotConfigTest, RejectsMissingEmptyOversizedAndInvalidJson)
{
    linux_hotspot_cfg_t config;
    char error[128];
    EXPECT_EQ(linux_hotspot_cfg_load(&config, "run/does-not-exist.json", error, sizeof(error)), DEMO_ERR_INVAL);
    EXPECT_NE(std::strstr(error, "does-not-exist.json"), nullptr);
    WriteConfig("");
    EXPECT_EQ(linux_hotspot_cfg_load(&config, kTempConfig, error, sizeof(error)), DEMO_ERR_INVAL);
    WriteConfig("not json");
    EXPECT_EQ(linux_hotspot_cfg_load(&config, kTempConfig, error, sizeof(error)), DEMO_ERR_INVAL);
    WriteConfig("[]");
    EXPECT_EQ(linux_hotspot_cfg_load(&config, kTempConfig, error, sizeof(error)), DEMO_ERR_INVAL);
    WriteConfig(std::string(1024 * 1024 + 1, 'x'));
    EXPECT_EQ(linux_hotspot_cfg_load(&config, kTempConfig, error, sizeof(error)), DEMO_ERR_INVAL);
    EXPECT_EQ(config.enable, 0);
}

TEST_F(LinuxHotspotConfigTest, RejectsWrongTypesAndTruncatedStrings)
{
    linux_hotspot_cfg_t config;
    char error[128];
    WriteConfig(R"({"nat":1})");
    EXPECT_EQ(linux_hotspot_cfg_load(&config, kTempConfig, error, sizeof(error)), DEMO_ERR_INVAL);
    EXPECT_NE(std::strstr(error, "nat"), nullptr);
    WriteConfig(std::string("{\"ap_interface\":\"") + std::string(32, 'a') + "\"}");
    EXPECT_EQ(linux_hotspot_cfg_load(&config, kTempConfig, error, sizeof(error)), DEMO_ERR_INVAL);
    EXPECT_NE(std::strstr(error, "ap_interface"), nullptr);
}

TEST_F(LinuxHotspotConfigTest, ValidatesPrefixesInterfacesAndAddresses)
{
    char error[128];
    linux_hotspot_cfg_t config = Defaults();
    config.prefix_length = 15;
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
    config = Defaults();
    std::strcpy(config.ap_interface, "ap bad");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
    config = Defaults();
    std::strcpy(config.subnet, "999.1.1.1");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
    config = Defaults();
    std::strcpy(config.subnet, "10.42.0.0");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
    std::strcpy(config.subnet, "10.42.0.255");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
}

TEST_F(LinuxHotspotConfigTest, RejectsInvalidDhcpPools)
{
    char error[128];
    linux_hotspot_cfg_t config = Defaults();
    std::strcpy(config.dhcp_start, "10.42.0.201");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
    config = Defaults();
    std::strcpy(config.dhcp_end, "10.43.0.200");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
    config = Defaults();
    std::strcpy(config.dhcp_start, "10.42.0.0");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
    config = Defaults();
    std::strcpy(config.dhcp_end, "10.42.0.255");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
    config = Defaults();
    std::strcpy(config.dhcp_start, "10.42.0.1");
    EXPECT_EQ(linux_hotspot_cfg_validate(&config, error, sizeof(error)), DEMO_ERR_INVAL);
}

TEST_F(LinuxHotspotConfigTest, SelectsFirstAvailableCandidateAndRelocatesPool)
{
    linux_hotspot_cfg_t config = Defaults();
    linux_ipv4_route_t routes[] = {Route("10.42.0.128", 25)};
    linux_hotspot_plan_t plan;
    char error[128];
    ASSERT_EQ(linux_hotspot_select_plan(&config, routes, 1, &plan, error, sizeof(error)), DEMO_OK) << error;
    EXPECT_EQ(plan.candidate_index, 1);
    EXPECT_EQ(plan.used_fallback, 1);
    EXPECT_EQ(plan.ap.address, Ip("10.43.0.1"));
    EXPECT_EQ(plan.dhcp_start, Ip("10.43.0.100"));
    EXPECT_EQ(plan.dhcp_end, Ip("10.43.0.200"));
}

TEST_F(LinuxHotspotConfigTest, DetectsWideAndNarrowRouteOverlap)
{
    linux_hotspot_cfg_t config = Defaults();
    linux_hotspot_plan_t plan;
    char error[128];
    linux_ipv4_route_t wide[] = {Route("10.0.0.0", 8)};
    ASSERT_EQ(linux_hotspot_select_plan(&config, wide, 1, &plan, error, sizeof(error)), DEMO_OK);
    EXPECT_EQ(plan.candidate_index, 2);
    linux_ipv4_route_t narrow[] = {Route("10.42.0.200", 32)};
    ASSERT_EQ(linux_hotspot_select_plan(&config, narrow, 1, &plan, error, sizeof(error)), DEMO_OK);
    EXPECT_EQ(plan.candidate_index, 1);
}

TEST_F(LinuxHotspotConfigTest, FailsClosedWhenAllCandidatesConflict)
{
    linux_hotspot_cfg_t config = Defaults();
    linux_ipv4_route_t routes[] = {
        Route("10.42.0.0", 24), Route("10.43.0.0", 24),
        Route("172.31.250.0", 24), Route("192.168.250.0", 24)
    };
    linux_hotspot_plan_t plan;
    char error[128];
    EXPECT_EQ(linux_hotspot_select_plan(&config, routes, 4, &plan, error, sizeof(error)), DEMO_ERR);
    EXPECT_EQ(plan.ap.address, 0U);
    EXPECT_NE(std::strstr(error, "候选"), nullptr);
}

TEST_F(LinuxHotspotConfigTest, SkipsFallbacksThatCannotContainPreferredOffsets)
{
    linux_hotspot_cfg_t config = Defaults();
    config.prefix_length = 16;
    std::strcpy(config.subnet, "10.42.0.1");
    std::strcpy(config.dhcp_start, "10.42.2.100");
    std::strcpy(config.dhcp_end, "10.42.2.200");
    linux_ipv4_route_t route = Route("10.42.0.0", 16);
    linux_hotspot_plan_t plan;
    char error[128];
    EXPECT_EQ(linux_hotspot_select_plan(&config, &route, 1, &plan, error, sizeof(error)), DEMO_ERR);
    EXPECT_EQ(plan.candidate_index, 0);
}

TEST_F(LinuxHotspotConfigTest, RejectsNullOutputsAndTerminatesErrors)
{
    linux_hotspot_cfg_t config = Defaults();
    linux_hotspot_plan_t plan;
    char error[4] = {'x', 'x', 'x', 'x'};
    EXPECT_EQ(linux_hotspot_select_plan(&config, nullptr, 1, &plan, error, sizeof(error)), DEMO_ERR_INVAL);
    EXPECT_EQ(error[3], '\0');
    EXPECT_EQ(linux_hotspot_select_plan(&config, nullptr, 0, nullptr, error, sizeof(error)), DEMO_ERR_INVAL);
}

}  // namespace
