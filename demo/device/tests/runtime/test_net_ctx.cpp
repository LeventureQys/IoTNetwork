#include "test_host_fixture.h"

#include "net_abstraction.h"

#include <cstring>

namespace {

/* 全空 vtable（能力回调可空，wrapper 安全返回 DEMO_ERR） */
static const net_backend_t empty_vtable = {0};

/* 无 init 的 vtable */
static const net_backend_t no_init_vtable = {
    NULL, /* init */
    NULL, /* deinit */
};

} // namespace

TEST(NetCtxContract, NewSignatureZeroesOut)
{
    net_ctx_t *ctx = (net_ctx_t *)(uintptr_t)0x1234;
    int rc = net_ctx_create(&empty_vtable, nullptr, nullptr, &ctx);
    EXPECT_EQ(rc, DEMO_OK);
    EXPECT_NE(ctx, nullptr);
    /* 进入时置 NULL 已验证（非零哨兵被覆盖）；成功路径 ctx 非空 */
    net_ctx_destroy(ctx);
}

TEST(NetCtxContract, NullArgs)
{
    net_ctx_t *ctx = (net_ctx_t *)(uintptr_t)1;
    EXPECT_EQ(net_ctx_create(nullptr, nullptr, nullptr, &ctx), DEMO_ERR_INVAL);
    EXPECT_EQ(ctx, nullptr); /* 进入置 NULL */
    EXPECT_EQ(net_ctx_create(&empty_vtable, nullptr, nullptr, nullptr), DEMO_ERR_INVAL);
}

TEST(NetCtxContract, InitFailReturnsOriginalErrorAndFrees)
{
    fake_backend_reset();
    fake_backend_set_init_fail(1);
    device_backend_instance_t inst;
    device_error_t e;
    device_sim_backend_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.nvs_file = "run/net_ctx_init_fail.nvs.json";
    ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
    net_ctx_t *ctx = (net_ctx_t *)(uintptr_t)1;
    int rc = net_ctx_create(inst.vtable, inst.user, nullptr, &ctx);
    EXPECT_EQ(rc, DEMO_ERR); /* 原样返回 init 错误 */
    EXPECT_EQ(ctx, nullptr); /* 失败释放，无残留 */
    EXPECT_EQ(fake_backend_deinit_calls(), 0); /* 未初始化不调 deinit */
    device_backend_instance_destroy(&inst);
    fake_backend_set_init_fail(0);
}

TEST(NetCtxContract, InitNullNoDeinit)
{
    fake_backend_reset();
    device_backend_instance_t inst;
    device_error_t e;
    device_sim_backend_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.nvs_file = "run/net_ctx_noinit.nvs.json";
    ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(&no_init_vtable, inst.user, nullptr, &ctx), DEMO_OK);
    EXPECT_NE(ctx, nullptr);
    net_ctx_destroy(ctx);
    EXPECT_EQ(fake_backend_deinit_calls(), 0);
    device_backend_instance_destroy(&inst);
}

TEST(NetCtxContract, InitOkDestroyCallsDeinitOnce)
{
    fake_backend_reset();
    device_backend_instance_t inst;
    device_error_t e;
    device_sim_backend_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.nvs_file = "run/net_ctx_deinit.nvs.json";
    ASSERT_EQ(device_sim_backend_create(&opts, &inst, &e), DEVICE_OK);
    net_ctx_t *ctx = nullptr;
    EXPECT_EQ(net_ctx_create(inst.vtable, inst.user, "cfg.json", &ctx), DEMO_OK);
    EXPECT_NE(ctx, nullptr);
    net_ctx_destroy(ctx);
    EXPECT_EQ(fake_backend_deinit_calls(), 1);
    net_ctx_destroy(nullptr); /* 幂等 */
    device_backend_instance_destroy(&inst);
}

TEST(NetCtxContract, NullCtxSafe)
{
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
    net_mdns_service_t svc;
    memset(&svc, 0, sizeof(svc));
    EXPECT_EQ(net_mdns_register(nullptr, &svc), DEMO_ERR_INVAL);
    int len = 4;
    uint8_t buf[4];
    EXPECT_EQ(net_nvs_get(nullptr, "k", buf, &len), DEMO_ERR_INVAL);
    EXPECT_EQ(net_time_ms(nullptr), 0u);
    EXPECT_EQ(net_random(nullptr), 0u);
    EXPECT_EQ(net_inject(nullptr, "a", nullptr), DEMO_ERR_INVAL);
}

TEST(NetCtxContract, NullMemberTableSafe)
{
    net_ctx_t *ctx = nullptr;
    ASSERT_EQ(net_ctx_create(&empty_vtable, nullptr, nullptr, &ctx), DEMO_OK);
    ASSERT_NE(ctx, nullptr);
    net_ap_info_t aps[2];
    int count = 2;
    EXPECT_EQ(net_wifi_scan(ctx, aps, &count), DEMO_ERR_INVAL);
    uint32_t gateway = 123;
    EXPECT_EQ(net_wifi_get_gateway(ctx, &gateway), DEMO_ERR_INVAL);
    EXPECT_EQ(gateway, 0u);
    void *sock = nullptr;
    EXPECT_EQ(net_tcp_listen(ctx, 1, &sock), DEMO_ERR_INVAL);
    EXPECT_EQ(net_time_ms(ctx), 0u);
    EXPECT_EQ(net_random(ctx), 0u);
    net_ctx_destroy(ctx);
}
