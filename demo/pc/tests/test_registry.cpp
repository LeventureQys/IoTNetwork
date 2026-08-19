#include <gtest/gtest.h>
#include <cstring>
#include "host_registry.h"
#include "host_tcp_server.h"
#include "params.h"
#include "log.h"

TEST(Registry, AddFindRemove)
{
    HostRegistry reg;
    DeviceEntry *e = reg.Add("02:00:00:00:00:01", (void *)1);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(reg.Size(), (size_t)1);
    EXPECT_EQ(reg.Find("02:00:00:00:00:01"), e);
    reg.Remove("02:00:00:00:00:01");
    EXPECT_EQ(reg.Size(), (size_t)0);
    EXPECT_EQ(reg.Find("02:00:00:00:00:01"), nullptr);
}

TEST(Registry, DuplicateReplace)
{
    HostRegistry reg;
    DeviceEntry *e1 = reg.Add("dev", (void *)1);
    DeviceEntry *e2 = reg.Add("dev", (void *)2);
    EXPECT_EQ(e1, e2); /* 同一条目替换 */
    EXPECT_EQ(reg.Size(), (size_t)1);
    EXPECT_EQ(reg.Find("dev")->conn, (void *)2);
}

TEST(Registry, PingLossStats)
{
    HostRegistry reg;
    reg.Add("dev", nullptr);
    reg.OnPing("dev", 1);
    reg.OnPing("dev", 2);
    reg.OnPing("dev", 5); /* 缺口 3,4 → lost=2 */
    EXPECT_EQ(reg.Find("dev")->lost_ping_count, 2u);
    reg.OnPing("dev", 3); /* 乱序忽略 */
    EXPECT_EQ(reg.Find("dev")->lost_ping_count, 2u);
}

TEST(Registry, FindDead)
{
    HostRegistry reg;
    reg.Add("a", nullptr);
    reg.Add("b", nullptr);
    reg.OnRx("a", 1000);
    reg.OnRx("b", 9000);
    auto dead = reg.FindDead(10000, 5000);
    ASSERT_EQ(dead.size(), (size_t)1);
    EXPECT_EQ(dead[0], "a");
    reg.MarkOffline("a");
    EXPECT_EQ(reg.Find("a")->state, "offline");
    /* offline 不再判死 */
    EXPECT_EQ(reg.FindDead(10000, 5000).size(), (size_t)0);
}

TEST(Registry, SessionResumeCount)
{
    HostRegistry reg;
    reg.Add("dev", nullptr);
    reg.OnRx("dev", 100);
    reg.Find("dev")->session_id = "abcd1234";
    reg.MarkOffline("dev");
    DeviceEntry *e2 = reg.Add("dev", nullptr);
    EXPECT_EQ(e2->state, "online");
    e2->reconnect_count = reg.Find("dev")->reconnect_count + 1;
    EXPECT_EQ(e2->reconnect_count, 1);
}
