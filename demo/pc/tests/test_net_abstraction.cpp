#include <gtest/gtest.h>
#include "net_abstraction.h"
#include "params.h"
#include "sim_backend.h"
#include "sim_world.h"

namespace {

int fail_init(void *, const char *) { return DEMO_ERR; }
void ok_deinit(void *) {}

const net_backend_t fail_init_table = {
    fail_init, ok_deinit, /* init / deinit */
    nullptr, nullptr, nullptr,                                  /* wifi scan/sta */
    nullptr, nullptr, nullptr, nullptr,                         /* ap start/stop/status/configure */
    nullptr, nullptr, nullptr, nullptr,                         /* rssi/get_ip/ssid/gateway */
    nullptr, nullptr, nullptr,                                  /* tcp */
    nullptr, nullptr, nullptr,                                  /* sock */
    nullptr, nullptr, nullptr,                                  /* udp */
    nullptr, nullptr, nullptr,                                  /* mdns */
    nullptr, nullptr, nullptr,                                  /* nvs */
    nullptr, nullptr,                                          /* time/random */
    nullptr,                                                   /* inject */
};

int ok_init(void *, const char *) { return DEMO_OK; }
int g_deinit_calls = 0;
void count_deinit(void *) { g_deinit_calls++; }

const net_backend_t ok_init_table = {
    ok_init, count_deinit,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr,
    nullptr, nullptr,
    nullptr,
};

} // namespace

TEST(NetAbstraction, NullCtxSafe)
{
    /* NULL ctx 安全返回 */
    net_ap_info_t aps[4];
    int count = 4;
    EXPECT_EQ(net_wifi_scan(nullptr, aps, &count), DEMO_ERR_INVAL);
    wifi_reason_t reason = WIFI_REASON_OK;
    EXPECT_EQ(net_wifi_sta_connect(nullptr, "s", "p", &reason), DEMO_ERR_INVAL);
    uint32_t gateway = 123;
    EXPECT_EQ(net_wifi_get_gateway(nullptr, &gateway), DEMO_ERR_INVAL);
    void *sock = nullptr;
    EXPECT_EQ(net_tcp_listen(nullptr, 5935, &sock), DEMO_ERR_INVAL);
    EXPECT_EQ(net_tcp_connect(nullptr, nullptr, &sock, 100), DEMO_ERR_INVAL);
    EXPECT_EQ(net_sock_send(nullptr, nullptr, nullptr, 0), DEMO_ERR_INVAL);
    net_sock_close(nullptr, nullptr); /* 不应崩溃 */
    EXPECT_EQ(net_udp_mcast_join(nullptr, "224.0.2.1", 5936, &sock), DEMO_ERR_INVAL);
    net_mdns_service_t svc{};
    EXPECT_EQ(net_mdns_register(nullptr, &svc), DEMO_ERR_INVAL);
    int len = 4;
    uint8_t buf[4];
    EXPECT_EQ(net_nvs_get(nullptr, "k", buf, &len), DEMO_ERR_INVAL);
    EXPECT_EQ(net_time_ms(nullptr), 0u);
    EXPECT_EQ(net_random(nullptr), 0u);
    EXPECT_EQ(net_inject(nullptr, "a", nullptr), DEMO_ERR_INVAL);

    net_ap_status_t status;
    EXPECT_EQ(net_wifi_ap_status(nullptr, &status), DEMO_ERR);
    EXPECT_EQ(net_wifi_ap_status(nullptr, nullptr), DEMO_ERR_INVAL);
    EXPECT_EQ(net_wifi_ap_configure_ipv4(nullptr, "192.168.137.1", 24), DEMO_ERR);
    EXPECT_EQ(net_wifi_ap_configure_ipv4(nullptr, nullptr, 24), DEMO_ERR_INVAL);
}

TEST(NetAbstraction, NullMemberTableSafe)
{
    /* 函数指针为 NULL 的后端表调用不崩溃 */
    net_backend_t be{};
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(&be, nullptr, nullptr, &ctx), DEMO_OK);
    ASSERT_NE(ctx, nullptr);
    net_ap_info_t aps[2];
    int count = 2;
    EXPECT_EQ(net_wifi_scan(ctx, aps, &count), DEMO_ERR_INVAL);
    uint32_t gateway = 123;
    EXPECT_EQ(net_wifi_get_gateway(ctx, &gateway), DEMO_ERR_INVAL);
    EXPECT_EQ(gateway, 0u);
    void *sock = nullptr;
    EXPECT_EQ(net_tcp_listen(ctx, 1, &sock), DEMO_ERR_INVAL);
    net_ap_status_t status;
    EXPECT_EQ(net_wifi_ap_status(ctx, &status), DEMO_ERR); /* 未实现 → DEMO_ERR */
    EXPECT_EQ(net_wifi_ap_configure_ipv4(ctx, "192.168.137.1", 24), DEMO_ERR);
    net_ctx_destroy(ctx);
}

TEST(NetAbstraction, CreateDestroy)
{
    net_backend_t be{};
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(&be, nullptr, nullptr, &ctx), DEMO_OK);
    ASSERT_NE(ctx, nullptr);
    net_ctx_destroy(ctx);
    net_ctx_destroy(nullptr); /* 不应崩溃 */
    int rc = net_ctx_create(nullptr, nullptr, nullptr, &ctx);
    EXPECT_EQ(rc, DEMO_ERR_INVAL);
    EXPECT_EQ(ctx, nullptr);
    rc = net_ctx_create(&be, nullptr, nullptr, nullptr);
    EXPECT_EQ(rc, DEMO_ERR_INVAL);
}

TEST(NetAbstraction, CreateSetsOutNullOnEntry)
{
    /* 进入先置 *out_ctx=NULL：失败时调用方拿到的必须为空 */
    net_ctx_t *ctx = (net_ctx_t *)0x1234;
    int rc = net_ctx_create(&fail_init_table, nullptr, nullptr, &ctx);
    EXPECT_EQ(rc, DEMO_ERR);
    EXPECT_EQ(ctx, nullptr);
}

TEST(NetAbstraction, BackendInitFailurePropagates)
{
    /* init 非空且失败：释放 ctx 并返回原错误 */
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(&fail_init_table, nullptr, nullptr, &ctx), DEMO_ERR);
    EXPECT_EQ(ctx, nullptr);
}

TEST(NetAbstraction, InitOkDeinitOnDestroy)
{
    /* 仅 init 成功后才标记 initialized；destroy 对已初始化后端调用 deinit */
    g_deinit_calls = 0;
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(&ok_init_table, nullptr, nullptr, &ctx), DEMO_OK);
    ASSERT_NE(ctx, nullptr);
    net_ctx_destroy(ctx);
    EXPECT_EQ(g_deinit_calls, 1);
}

TEST(NetAbstraction, SimGatewayFollowsConnectedAp)
{
    demo_params_t params;
    params_defaults(&params);
    SimWorld::Instance().ApUnregister("gateway_test");
    SimWorld::Instance().ApRegister("gateway_test", "Modu_GATE", "modutech_leventure",
                                    "5935", 22000);
    void *user = sim_backend_create("host", &params);
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(sim_backend_table(), user, nullptr, &ctx), DEMO_OK);
    ASSERT_NE(ctx, nullptr);
    wifi_reason_t reason = WIFI_REASON_OK;
    ASSERT_EQ(net_wifi_sta_connect(ctx, "Modu_GATE", "modutech_leventure", &reason),
              DEMO_OK);
    uint32_t gateway = 0;
    EXPECT_EQ(net_wifi_get_gateway(ctx, &gateway), DEMO_OK);
    EXPECT_EQ(gateway, SimWorld::DeviceApVirtualIp());
    net_ctx_destroy(ctx);
    sim_backend_destroy(user);
    SimWorld::Instance().ApUnregister("gateway_test");
}
