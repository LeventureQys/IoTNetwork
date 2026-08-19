/* WinHotspot 可注入 ops 单元测试：覆盖成功/能力缺失/权限/超时错误映射。
 * 仅 Windows 编译（WinHotspot 实现位于 pc_win_backend）。 */
#ifdef _WIN32

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include "win_hotspot.h"
#include "common.h"

namespace {

struct FakeCtx {
    std::string last_error;
    int start_rc = DEMO_OK;
    int query_rc = DEMO_OK;
    int configure_rc = DEMO_OK;
    int stop_rc = DEMO_OK;
    net_ap_status_t status{};
};

int fake_start(void *ctx, const char *, const char *, std::string *error)
{
    auto *c = static_cast<FakeCtx *>(ctx);
    if (error)
        *error = c->last_error;
    return c->start_rc;
}

int fake_query(void *ctx, net_ap_status_t *status, std::string *error)
{
    auto *c = static_cast<FakeCtx *>(ctx);
    if (error)
        *error = c->last_error;
    if (c->query_rc == DEMO_OK)
        *status = c->status;
    return c->query_rc;
}

int fake_configure(void *ctx, const char *, int, std::string *error)
{
    auto *c = static_cast<FakeCtx *>(ctx);
    if (error)
        *error = c->last_error;
    return c->configure_rc;
}

int fake_stop(void *ctx, std::string *error)
{
    auto *c = static_cast<FakeCtx *>(ctx);
    if (error)
        *error = c->last_error;
    return c->stop_rc;
}

/* 注入模式不持有 ctx：create/destroy 不会被调用。 */
const WinHotspotOps g_fake_ops = {
    nullptr, nullptr, fake_start, fake_query, fake_configure, fake_stop,
};

} // namespace

TEST(WinHotspot, StartSuccess)
{
    FakeCtx ctx;
    ctx.start_rc = DEMO_OK;
    WinHotspot hotspot(&g_fake_ops, &ctx);
    std::string error;
    EXPECT_EQ(hotspot.Start("Modu_PC", "modu_leventure", &error), DEMO_OK);
}

TEST(WinHotspot, CapabilityMissingMapsToErr)
{
    FakeCtx ctx;
    ctx.start_rc = DEMO_ERR;
    ctx.last_error = "系统移动热点能力不可用（组策略/硬件/SKU 限制）";
    WinHotspot hotspot(&g_fake_ops, &ctx);
    std::string error;
    EXPECT_EQ(hotspot.Start("Modu_PC", "modu_leventure", &error), DEMO_ERR);
    EXPECT_NE(error.find("能力不可用"), std::string::npos);
}

TEST(WinHotspot, PermissionDeniedMapsToErr)
{
    FakeCtx ctx;
    ctx.start_rc = DEMO_ERR;
    ctx.last_error = "移动热点权限不足";
    WinHotspot hotspot(&g_fake_ops, &ctx);
    std::string error;
    EXPECT_EQ(hotspot.Start("Modu_PC", "modu_leventure", &error), DEMO_ERR);
    EXPECT_NE(error.find("权限不足"), std::string::npos);
}

TEST(WinHotspot, StartTimeoutMapsToTimeout)
{
    FakeCtx ctx;
    ctx.start_rc = DEMO_ERR_TIMEOUT;
    ctx.last_error = "等待移动热点就绪超时";
    WinHotspot hotspot(&g_fake_ops, &ctx);
    std::string error;
    EXPECT_EQ(hotspot.Start("Modu_PC", "modu_leventure", &error), DEMO_ERR_TIMEOUT);
}

TEST(WinHotspot, QuerySuccessFillsStatus)
{
    FakeCtx ctx;
    ctx.query_rc = DEMO_OK;
    ctx.status.started = 1;
    snprintf(ctx.status.ssid, sizeof(ctx.status.ssid), "Modu_PC");
    snprintf(ctx.status.ipv4, sizeof(ctx.status.ipv4), "192.168.137.1");
    ctx.status.prefix_length = 24;
    WinHotspot hotspot(&g_fake_ops, &ctx);
    net_ap_status_t status;
    std::string error;
    EXPECT_EQ(hotspot.Query(&status, &error), DEMO_OK);
    EXPECT_EQ(status.started, 1);
    EXPECT_STREQ(status.ipv4, "192.168.137.1");
}

TEST(WinHotspot, QueryNotRunningMapsToErr)
{
    FakeCtx ctx;
    ctx.query_rc = DEMO_ERR;
    ctx.last_error = "热点未运行";
    WinHotspot hotspot(&g_fake_ops, &ctx);
    net_ap_status_t status;
    std::string error;
    EXPECT_EQ(hotspot.Query(&status, &error), DEMO_ERR);
}

TEST(WinHotspot, ConfigureSuccessAndFailure)
{
    FakeCtx ctx;
    ctx.configure_rc = DEMO_OK;
    WinHotspot hotspot(&g_fake_ops, &ctx);
    std::string error;
    EXPECT_EQ(hotspot.ConfigureIpv4("192.168.137.1", 24, &error), DEMO_OK);

    ctx.configure_rc = DEMO_ERR;
    ctx.last_error = "设置热点承载适配器 IPv4 失败";
    EXPECT_EQ(hotspot.ConfigureIpv4("192.168.137.1", 24, &error), DEMO_ERR);
}

TEST(WinHotspot, StopReturnsOk)
{
    FakeCtx ctx;
    ctx.stop_rc = DEMO_OK;
    WinHotspot hotspot(&g_fake_ops, &ctx);
    std::string error;
    EXPECT_EQ(hotspot.Stop(&error), DEMO_OK);
}

#endif // _WIN32
