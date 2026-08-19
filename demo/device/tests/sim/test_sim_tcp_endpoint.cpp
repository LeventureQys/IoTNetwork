#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include "sim_tcp_endpoint.h"
#include "sim_world.h"
#include "net_abstraction.h"

namespace {

const uint32_t kLoopbackIpNetworkOrder = htonl(INADDR_LOOPBACK);

std::string ip_str(uint32_t ip_network_order)
{
    const uint8_t *b = (const uint8_t *)&ip_network_order;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

net_addr_t make_addr(const char *ip, int port)
{
    net_addr_t a;
    a.ip = inet_addr(ip);
    a.port = (uint16_t)(((uint16_t)port << 8) | ((uint16_t)port >> 8));
    return a;
}

} // namespace

TEST(SimTcpEndpoint, RealDeviceKeepsRequestedEndpoint)
{
    net_addr_t requested = make_addr("192.168.1.191", 5935);
    sim_tcp_endpoint_t ep = sim_tcp_resolve_endpoint(
        1, &requested, 0, 0, sim_world_host_virtual_ip(), 5935);
    EXPECT_EQ(ep.ip, requested.ip);
    EXPECT_EQ(ep.port, requested.port);
    EXPECT_EQ(ip_str(ep.ip), "192.168.1.191");
    EXPECT_EQ((unsigned)ntohs(ep.port), 5935u);
}

TEST(SimTcpEndpoint, RealDeviceIgnoresSimTranslation)
{
    /* 真实模式即使传入模拟 AP/Host 匹配信息也必须保持请求端点 */
    net_addr_t requested = make_addr("10.0.0.8", 9000);
    sim_tcp_endpoint_t ep = sim_tcp_resolve_endpoint(
        1, &requested, 1, 21000, inet_addr("192.168.1.50"), 5935);
    EXPECT_EQ(ep.ip, requested.ip);
    EXPECT_EQ(ep.port, requested.port);
    EXPECT_EQ(ip_str(ep.ip), "10.0.0.8");
    EXPECT_EQ((unsigned)ntohs(ep.port), 9000u);
}

TEST(SimTcpEndpoint, SimHostTranslationToLoopback)
{
    net_addr_t requested = make_addr("192.168.1.50", 5935);
    sim_tcp_endpoint_t ep = sim_tcp_resolve_endpoint(
        0, &requested, 0, 0, inet_addr("192.168.1.50"), 5935);
    EXPECT_EQ(ep.ip, kLoopbackIpNetworkOrder);
    EXPECT_EQ(ip_str(ep.ip), "127.0.0.1");
    EXPECT_EQ((unsigned)ntohs(ep.port), 5935u);
}

TEST(SimTcpEndpoint, SimApTranslationToLoopbackRealPort)
{
    net_addr_t requested = make_addr("192.168.1.1", 5935);
    sim_tcp_endpoint_t ep = sim_tcp_resolve_endpoint(
        0, &requested, 1, 21000, inet_addr("192.168.1.50"), 5935);
    EXPECT_EQ(ep.ip, kLoopbackIpNetworkOrder);
    EXPECT_EQ(ip_str(ep.ip), "127.0.0.1");
    EXPECT_EQ((unsigned)ntohs(ep.port), 21000u);
}

TEST(SimTcpEndpoint, SimOtherAddressKeepsRequestedPort)
{
    net_addr_t requested = make_addr("10.1.2.3", 7000);
    sim_tcp_endpoint_t ep = sim_tcp_resolve_endpoint(
        0, &requested, 0, 0, inet_addr("192.168.1.50"), 5935);
    EXPECT_EQ(ep.ip, kLoopbackIpNetworkOrder);
    EXPECT_EQ(ip_str(ep.ip), "127.0.0.1");
    EXPECT_EQ(ep.port, requested.port);
    EXPECT_EQ((unsigned)ntohs(ep.port), 7000u);
}

TEST(SimTcpEndpoint, NullRequestedReturnsZeroEndpoint)
{
    sim_tcp_endpoint_t ep = sim_tcp_resolve_endpoint(0, nullptr, 0, 0, 0, 0);
    EXPECT_EQ(ep.ip, 0u);
    EXPECT_EQ(ep.port, 0u);
}
