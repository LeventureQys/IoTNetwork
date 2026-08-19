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
    SimWorld &w = SimWorld::Instance();
    w.ApRegister("dev0", "Modu_0001", "pass123456", "1234", 20000);
    SimAp ap;
    EXPECT_TRUE(w.ApFind("Modu_0001", &ap));
    EXPECT_STREQ(ap.pin, "1234");
    EXPECT_EQ(ap.real_port, 20000);
    /* 重复注册同 owner 覆盖 */
    w.ApRegister("dev0", "Modu_0001", "pass123456", "5678", 20000);
    EXPECT_TRUE(w.ApFind("Modu_0001", &ap));
    EXPECT_STREQ(ap.pin, "5678");
    EXPECT_EQ(w.ApCount(), (size_t)1);
    w.ApUnregister("dev0");
    EXPECT_FALSE(w.ApFind("Modu_0001", &ap));
    EXPECT_EQ(w.ApCount(), (size_t)0);
}

TEST(SimWorld, StaConnectReasons)
{
    SimWorld &w = SimWorld::Instance();
    w.TargetNetworkSet("TestWifi", "secret123", true);
    EXPECT_EQ(w.StaConnect("TestWifi", "secret123"), 0);       /* ok */
    EXPECT_EQ(w.StaConnect("OtherNet", "secret123"), 201);     /* NO_AP_FOUND */
    EXPECT_EQ(w.StaConnect("TestWifi", "wrongpass"), 202);     /* AUTH_FAIL */
    w.TargetSetUp(false);
    EXPECT_EQ(w.StaConnect("TestWifi", "secret123"), 201);     /* 网络 down */
    w.TargetSetUp(true);
    w.TargetSetAuthFail(true);
    EXPECT_EQ(w.StaConnect("TestWifi", "secret123"), 202);     /* 注入认证失败 */
    w.TargetSetAuthFail(false);
    /* 5G 网络 */
    w.TargetNetworkSet("TestWifi", "secret123", false);
    EXPECT_EQ(w.StaConnect("TestWifi", "secret123"), 500);
    w.TargetNetworkSet("TestWifi", "secret123", true);
}

TEST(SimWorld, RssiInject)
{
    SimWorld &w = SimWorld::Instance();
    w.RssiSet("dev0", -80);
    EXPECT_EQ(w.RssiGet("dev0"), -80);
    EXPECT_EQ(w.RssiGet("dev1"), -58); /* 默认 */
    w.RssiSet("dev0", -50);
    EXPECT_EQ(w.RssiGet("dev0"), -50);
}

TEST(SimWorld, MdnsRegisterResolve)
{
    SimWorld &w = SimWorld::Instance();
    SimMdnsSvc svc;
    memset(&svc, 0, sizeof(svc));
    snprintf(svc.instance, sizeof(svc.instance), "host");
    snprintf(svc.type, sizeof(svc.type), "_tactile._tcp");
    svc.ip = 0x3201A8C0; /* 192.168.1.50 网络字节序 */
    svc.port = htons(5935);
    svc.active = true;
    w.MdnsRegister(svc);
    SimMdnsSvc out;
    EXPECT_TRUE(w.MdnsResolve("_tactile._tcp", &out));
    EXPECT_EQ(out.ip, svc.ip);
    w.MdnsUnregister("_tactile._tcp");
    EXPECT_FALSE(w.MdnsResolve("_tactile._tcp", &out));
}

TEST(SimWorld, McastBlock)
{
    SimWorld &w = SimWorld::Instance();
    w.McastSetBlocked("dev0", true);
    EXPECT_TRUE(w.McastIsBlocked("dev0"));
    EXPECT_FALSE(w.McastIsBlocked("dev1"));
    w.McastSetBlocked("dev0", false);
    EXPECT_FALSE(w.McastIsBlocked("dev0"));
}

TEST(SimWorld, VirtualIps)
{
    EXPECT_EQ(SimWorld::HostVirtualIp(), inet_addr(PROTO_PC_AP_IP)); /* 192.168.137.1 */
    EXPECT_EQ(SimWorld::DeviceApVirtualIp(), inet_addr("192.168.1.1"));
    EXPECT_EQ(SimWorld::DeviceStaVirtualIp(0), inet_addr("192.168.1.100"));
    EXPECT_EQ(SimWorld::DeviceApRealPort(20000, 2), 20002);
}
