#include <gtest/gtest.h>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include "protocol.h"
#include "sim_world.h"

TEST(SimWorld, ApLifecycle)
{
    sim_world_t *w = sim_world_create();
    ASSERT_NE(w, nullptr);
    sim_world_ap_register(w, "dev0", "Modu_0001", "pass123456", "1234", 20000);
    sim_ap_t ap;
    EXPECT_TRUE(sim_world_ap_find(w, "Modu_0001", &ap));
    EXPECT_STREQ(ap.pin, "1234");
    EXPECT_EQ(ap.real_port, 20000);
    /* 重复注册同 owner 覆盖 */
    sim_world_ap_register(w, "dev0", "Modu_0001", "pass123456", "5678", 20000);
    EXPECT_TRUE(sim_world_ap_find(w, "Modu_0001", &ap));
    EXPECT_STREQ(ap.pin, "5678");
    EXPECT_EQ(sim_world_ap_count(w), 1);
    sim_world_ap_unregister(w, "dev0");
    EXPECT_FALSE(sim_world_ap_find(w, "Modu_0001", &ap));
    EXPECT_EQ(sim_world_ap_count(w), 0);
    sim_world_destroy(w);
}

TEST(SimWorld, WorldInstancesAreIsolated)
{
    sim_world_t *w1 = sim_world_create();
    sim_world_t *w2 = sim_world_create();
    ASSERT_NE(w1, nullptr);
    ASSERT_NE(w2, nullptr);
    sim_world_ap_register(w1, "dev0", "Modu_0001", "p1", "1234", 20000);
    sim_ap_t ap;
    EXPECT_TRUE(sim_world_ap_find(w1, "Modu_0001", &ap));
    EXPECT_FALSE(sim_world_ap_find(w2, "Modu_0001", &ap));
    EXPECT_EQ(sim_world_ap_count(w1), 1);
    EXPECT_EQ(sim_world_ap_count(w2), 0);
    sim_world_rssi_set(w1, "dev0", -80);
    EXPECT_EQ(sim_world_rssi_get(w1, "dev0"), -80);
    EXPECT_EQ(sim_world_rssi_get(w2, "dev0"), -58);
    sim_world_destroy(w1);
    sim_world_destroy(w2);
}

TEST(SimWorld, StaConnectReasons)
{
    sim_world_t *w = sim_world_create();
    ASSERT_NE(w, nullptr);
    sim_world_target_set(w, "TestWifi", "secret123", 1);
    EXPECT_EQ(sim_world_sta_connect(w, "TestWifi", "secret123"), 0);   /* ok */
    EXPECT_EQ(sim_world_sta_connect(w, "OtherNet", "secret123"), 201); /* NO_AP_FOUND */
    EXPECT_EQ(sim_world_sta_connect(w, "TestWifi", "wrongpass"), 202); /* AUTH_FAIL */
    sim_world_target_set_up(w, 0);
    EXPECT_EQ(sim_world_sta_connect(w, "TestWifi", "secret123"), 201); /* 网络 down */
    sim_world_target_set_up(w, 1);
    sim_world_target_set_auth_fail(w, 1);
    EXPECT_EQ(sim_world_sta_connect(w, "TestWifi", "secret123"), 202); /* 注入认证失败 */
    sim_world_target_set_auth_fail(w, 0);
    /* 5G 网络 */
    sim_world_target_set(w, "TestWifi", "secret123", 0);
    EXPECT_EQ(sim_world_sta_connect(w, "TestWifi", "secret123"), 500);
    sim_world_target_set(w, "TestWifi", "secret123", 1);
    sim_world_destroy(w);
}

TEST(SimWorld, RssiInject)
{
    sim_world_t *w = sim_world_create();
    ASSERT_NE(w, nullptr);
    sim_world_rssi_set(w, "dev0", -80);
    EXPECT_EQ(sim_world_rssi_get(w, "dev0"), -80);
    EXPECT_EQ(sim_world_rssi_get(w, "dev1"), -58); /* 默认 */
    sim_world_rssi_set(w, "dev0", -50);
    EXPECT_EQ(sim_world_rssi_get(w, "dev0"), -50);
    sim_world_destroy(w);
}

TEST(SimWorld, MdnsRegisterResolve)
{
    sim_world_t *w = sim_world_create();
    ASSERT_NE(w, nullptr);
    sim_mdns_svc_t svc;
    memset(&svc, 0, sizeof(svc));
    snprintf(svc.instance, sizeof(svc.instance), "host");
    snprintf(svc.type, sizeof(svc.type), "_tactile._tcp");
    svc.ip = 0x3201A8C0; /* 192.168.1.50 网络字节序 */
    svc.port = htons(5935);
    svc.active = 1;
    sim_world_mdns_register(w, &svc);
    sim_mdns_svc_t out;
    EXPECT_TRUE(sim_world_mdns_resolve(w, "_tactile._tcp", &out));
    EXPECT_EQ(out.ip, svc.ip);
    sim_world_mdns_unregister(w, "_tactile._tcp");
    EXPECT_FALSE(sim_world_mdns_resolve(w, "_tactile._tcp", &out));
    sim_world_destroy(w);
}

TEST(SimWorld, McastBlock)
{
    sim_world_t *w = sim_world_create();
    ASSERT_NE(w, nullptr);
    sim_world_mcast_set_blocked(w, "dev0", 1);
    EXPECT_TRUE(sim_world_mcast_is_blocked(w, "dev0"));
    EXPECT_FALSE(sim_world_mcast_is_blocked(w, "dev1"));
    sim_world_mcast_set_blocked(w, "dev0", 0);
    EXPECT_FALSE(sim_world_mcast_is_blocked(w, "dev0"));
    sim_world_destroy(w);
}

TEST(SimWorld, VirtualIps)
{
    EXPECT_EQ(sim_world_pc_ap_ip(), inet_addr("192.168.137.1"));
    EXPECT_EQ(sim_world_device_sta_virtual_ip(0), inet_addr("192.168.137.100"));
    EXPECT_EQ(sim_world_device_sta_virtual_ip(2), inet_addr("192.168.137.102"));
}
