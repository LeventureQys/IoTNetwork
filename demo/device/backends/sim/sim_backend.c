/* ============================================================================
 * sim_backend.c - 模拟后端上下文、29 项 vtable 与工厂装配（纯 C11）。
 *
 * 迁移自旧 net_sim/sim_backend.cpp：剥离全部 Linux 真实热点/STA 分支与
 * singleton；所有状态进入显式 sim_backend_t 实例；vtable 布局保持
 * net_abstraction.h 的 29 项不变。
 *
 * beta v1.1 角色反转（设计文档 7.3 / DQ6）：
 *   - 设备只作为 STA：扫描/连接/网关均来自 PC 发布的 catalog 记录；
 *     设备不再创建热点，wifi_ap_start/stop 返回 DEMO_ERR。
 *   - TCP 固定目标 192.168.137.1:5935 经 catalog 翻译到 127.0.0.1:<loopback_port>；
 *     其余地址不做该翻译。
 *
 * 公开契约（设计文档 8.2.1）：
 *   device_sim_backend_create(options, out_instance, error)
 *   - 进入时清零 out_instance；
 *   - 失败返回明确 device_result_t 且无部分实例；
 *   - 成功时 vtable/user/destroy_user 均非空；
 *   - options 字符串仅在调用期借用，内部复制。
 * 实例销毁（destroy_user）幂等：可接收 NULL。
 * ========================================================================== */
#include "device_backend_factory.h"
#include "sim_world.h"
#include "sim_tcp_endpoint.h"
#include "sim_socket.h"
#include "sim_nvs.h"
#include "sim_ap_catalog.h"
#include "sim_mdns.h"
#include "sim_fault.h"
#include "sim_random.h"
#include "sim_util.h"
#include "common.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>

typedef struct sim_backend {
    sim_world_t *world;
    sim_random_t *rng;
    sim_nvs_t *nvs;
    sim_fault_t fault;

    char tag[16];
    unsigned int device_index;

    int sta_connected;
    char sta_ssid[33];

    char catalog_dir[1024];
    char nvs_file[1024];

    int wsa_ready;
} sim_backend_t;

static void sim_backend_set_error(device_error_t *error, device_result_t code,
                                  const char *message)
{
    if (!error)
        return;
    error->code = code;
    sim_util_copy_bounded(error->message, sizeof(error->message), message);
}

static const char *sim_backend_nvs_default_path(const sim_backend_t *b)
{
    static char buf[1024];
    snprintf(buf, sizeof(buf), "run/dev%u.nvs.json", b->device_index);
    return buf;
}

/* ---------------- vtable 实现 ---------------- */

static int b_init(void *user, const char *config_path)
{
    (void)user;
    (void)config_path;
    /* WSA 与全部资源在 create 阶段完成；init 恒成功（与旧实现一致） */
    return DEMO_OK;
}

static void b_deinit(void *user)
{
    (void)user;
}

static int b_wifi_scan(void *user, net_ap_info_t *aps, int *count)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b || !count)
        return DEMO_ERR_INVAL;
    int cap = *count;
    int n = 0;

    /* PC 热点：catalog 存在有效记录时作为一条 2.4GHz AP 返回；无记录不返回 */
    sim_ap_catalog_record_t rec;
    if (sim_ap_catalog_read(b->catalog_dir, &rec, 0) == DEMO_OK) {
        if (n < cap && aps) {
            memset(&aps[n], 0, sizeof(aps[n]));
            sim_util_copy_bounded(aps[n].ssid, sizeof(aps[n].ssid), rec.ssid);
            aps[n].rssi = -50;
            aps[n].band_2g = 1;
        }
        n++;
    }
    *count = n;
    return DEMO_OK;
}

static int b_wifi_sta_connect(void *user, const char *ssid, const char *pass,
                              wifi_reason_t *reason)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b || !ssid || !pass)
        return DEMO_ERR_INVAL;

    /* 精确匹配 SSID；密码不符 → AUTH_FAIL；无有效记录 → NO_AP_FOUND */
    int r = WIFI_REASON_NO_AP_FOUND;
    sim_ap_catalog_record_t rec;
    if (sim_ap_catalog_read(b->catalog_dir, &rec, 0) == DEMO_OK) {
        if (strcmp(rec.ssid, ssid) == 0)
            r = (strcmp(rec.password, pass) == 0) ? WIFI_REASON_OK
                                                  : WIFI_REASON_AUTH_FAIL;
    }

    if (reason)
        *reason = (wifi_reason_t)r;
    if (r == WIFI_REASON_OK) {
        b->sta_connected = 1;
        sim_util_copy_bounded(b->sta_ssid, sizeof(b->sta_ssid), ssid);
    } else {
        b->sta_connected = 0;
        b->sta_ssid[0] = 0;
    }
    return r == WIFI_REASON_OK ? DEMO_OK : DEMO_ERR;
}

static int b_wifi_sta_disconnect(void *user)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    b->sta_connected = 0;
    b->sta_ssid[0] = 0;
    return DEMO_OK;
}

static int b_wifi_ap_start(void *user, const char *ssid, const char *pass, const char *pin)
{
    (void)user;
    (void)ssid;
    (void)pass;
    (void)pin;
    /* beta v1.1：设备不再创建热点（PC 是唯一 publisher），返回不支持 */
    return DEMO_ERR;
}

static int b_wifi_ap_stop(void *user)
{
    (void)user;
    /* beta v1.1：设备不管理热点 */
    return DEMO_ERR;
}

static int b_wifi_get_rssi(void *user, int *rssi)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b || !rssi)
        return DEMO_ERR_INVAL;
    *rssi = sim_world_rssi_get(b->world, b->tag);
    return DEMO_OK;
}

static int b_wifi_get_ip(void *user, uint32_t *ip)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b || !ip)
        return DEMO_ERR_INVAL;
    if (b->sta_connected)
        *ip = sim_world_device_sta_virtual_ip((int)b->device_index);
    else
        *ip = 0;
    return DEMO_OK;
}

static int b_wifi_get_current_ssid(void *user, char *ssid, int capacity)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b || !ssid || capacity <= 0 || !b->sta_connected || !b->sta_ssid[0])
        return DEMO_ERR;
    sim_util_copy_bounded(ssid, (size_t)capacity, b->sta_ssid);
    return DEMO_OK;
}

static int b_wifi_get_gateway(void *user, uint32_t *ip)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b || !ip || !b->sta_connected)
        return DEMO_ERR;
    *ip = sim_world_pc_ap_ip(); /* 192.168.137.1（PROTO_PC_AP_IP） */
    return DEMO_OK;
}

static int b_tcp_listen(void *user, uint16_t port, void **sock)
{
    (void)user;
    return sim_socket_tcp_listen(port, sock);
}

static int b_tcp_accept(void *user, void *listen, void **conn, net_addr_t *peer)
{
    (void)user;
    return sim_socket_tcp_accept(listen, conn, peer);
}

static int b_tcp_connect(void *user, const net_addr_t *addr, void **sock, int timeout_ms)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b || !addr)
        return DEMO_ERR_INVAL;

    /* PC 热点固定目标 192.168.137.1:5935 → 经 catalog 翻译到
     * 127.0.0.1:<loopback_port>；其余地址不做该翻译。 */
    int pc_match = (addr->ip == sim_world_pc_ap_ip() &&
                    addr->port == sim_tcp_host_to_net16((uint16_t)PROTO_TCP_PORT));
    uint16_t loopback_port = 0;
    if (pc_match) {
        sim_ap_catalog_record_t rec;
        if (sim_ap_catalog_read(b->catalog_dir, &rec, 0) != DEMO_OK)
            return DEMO_ERR; /* PC 未发布热点 → 固定目标无法翻译 */
        loopback_port = (uint16_t)rec.loopback_port;
    }

    sim_tcp_endpoint_t ep = sim_tcp_resolve_endpoint(0, addr, pc_match, loopback_port);
    net_addr_t resolved;
    resolved.ip = ep.ip;
    resolved.port = ep.port;
    return sim_socket_tcp_connect(&resolved, sock, timeout_ms);
}

static int b_sock_send(void *user, void *sock, const uint8_t *buf, int len)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    if (sim_fault_should_fail_send(&b->fault))
        return DEMO_ERR; /* sock_send_fail 注入 */
    return sim_socket_send(sock, buf, len);
}

static int b_sock_recv(void *user, void *sock, uint8_t *buf, int cap)
{
    (void)user;
    return sim_socket_recv(sock, buf, cap);
}

static void b_sock_close(void *user, void *sock)
{
    (void)user;
    sim_socket_close(sock);
}

static int b_udp_mcast_join(void *user, const char *group, uint16_t port, void **sock)
{
    (void)user;
    return sim_socket_udp_mcast_join(group, port, sock);
}

static int b_udp_send(void *user, const char *group, uint16_t port,
                      const uint8_t *buf, int len)
{
    (void)user;
    return sim_socket_udp_send(group, port, buf, len);
}

static int b_udp_recv(void *user, void *sock, uint8_t *buf, int cap, net_addr_t *from)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    int drain = sim_world_mcast_is_blocked(b->world, b->tag);
    return sim_socket_udp_recv(sock, buf, cap, from, drain);
}

static int b_mdns_register(void *user, const net_mdns_service_t *svc)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    return sim_mdns_register(b->world, svc);
}

static int b_mdns_unregister(void *user, const char *type)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    return sim_mdns_unregister(b->world, type);
}

static int b_mdns_resolve(void *user, const char *type, net_mdns_service_t *out,
                          int timeout_ms)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    return sim_mdns_resolve(b->world, type, out, timeout_ms);
}

static int b_nvs_get(void *user, const char *key, uint8_t *buf, int *len)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b || !key || !len || *len < 0 || (buf == NULL && *len > 0))
        return DEMO_ERR_INVAL;
    int out_len = 0;
    int rc = sim_nvs_get(b->nvs, key, buf, *len, &out_len);
    *len = out_len;
    return rc;
}

static int b_nvs_set(void *user, const char *key, const uint8_t *buf, int len)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    return sim_nvs_set(b->nvs, key, buf, len);
}

static int b_nvs_erase(void *user, const char *key)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    return sim_nvs_erase(b->nvs, key);
}

static uint64_t b_time_ms(void *user)
{
    (void)user;
    return sim_util_monotonic_ms();
}

static uint32_t b_random(void *user)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return 0;
    return sim_random_next(b->rng);
}

static int b_inject(void *user, const char *action, const char *arg_json)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return DEMO_ERR_INVAL;
    return sim_fault_inject(&b->fault, action, arg_json);
}

/* 29 项 vtable，布局与 net_abstraction.h 完全一致 */
static const net_backend_t g_sim_backend_vtable = {
    b_init, b_deinit,
    b_wifi_scan, b_wifi_sta_connect, b_wifi_sta_disconnect,
    b_wifi_ap_start, b_wifi_ap_stop, b_wifi_get_rssi, b_wifi_get_ip,
    b_wifi_get_current_ssid, b_wifi_get_gateway,
    b_tcp_listen, b_tcp_accept, b_tcp_connect,
    b_sock_send, b_sock_recv, b_sock_close,
    b_udp_mcast_join, b_udp_send, b_udp_recv,
    b_mdns_register, b_mdns_unregister, b_mdns_resolve,
    b_nvs_get, b_nvs_set, b_nvs_erase,
    b_time_ms, b_random,
    b_inject,
};

/* ---------------- 销毁 ---------------- */

static void sim_backend_destroy_user(void *user)
{
    sim_backend_t *b = (sim_backend_t *)user;
    if (!b)
        return;
    if (b->nvs)
        sim_nvs_close(b->nvs);
    if (b->world)
        sim_world_destroy(b->world);
    if (b->rng)
        sim_random_destroy(b->rng);
    if (b->wsa_ready)
        sim_socket_cleanup();
    free(b);
}

/* ---------------- 工厂 ---------------- */

device_result_t device_sim_backend_create(
    const device_sim_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error)
{
    if (out_instance)
        memset(out_instance, 0, sizeof(*out_instance));
    if (error) {
        memset(error, 0, sizeof(*error));
        error->code = DEVICE_OK;
        sim_util_copy_bounded(error->operation, sizeof(error->operation),
                              "device_sim_backend_create");
    }

    if (!options || !out_instance) {
        sim_backend_set_error(error, DEVICE_ERR_INVALID_ARGUMENT,
                              "options 或 out_instance 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (!options->sim_catalog_dir || !options->sim_catalog_dir[0]) {
        sim_backend_set_error(error, DEVICE_ERR_INVALID_ARGUMENT,
                              "sim_catalog_dir 不能为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (!sim_util_is_absolute(options->sim_catalog_dir)) {
        sim_backend_set_error(error, DEVICE_ERR_INVALID_ARGUMENT,
                              "sim_catalog_dir 必须为绝对路径");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (options->device_index > 15) {
        sim_backend_set_error(error, DEVICE_ERR_INVALID_ARGUMENT,
                              "device_index 必须在 0..15");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }

    sim_backend_t *b = (sim_backend_t *)calloc(1, sizeof(sim_backend_t));
    if (!b) {
        sim_backend_set_error(error, DEVICE_ERR_NO_MEMORY, "sim_backend 上下文分配失败");
        return DEVICE_ERR_NO_MEMORY;
    }
    b->device_index = options->device_index;
    snprintf(b->tag, sizeof(b->tag), "dev%u", options->device_index);
    sim_util_copy_bounded(b->catalog_dir, sizeof(b->catalog_dir), options->sim_catalog_dir);
    if (options->nvs_file && options->nvs_file[0])
        sim_util_copy_bounded(b->nvs_file, sizeof(b->nvs_file), options->nvs_file);
    else
        sim_util_copy_bounded(b->nvs_file, sizeof(b->nvs_file),
                              sim_backend_nvs_default_path(b));

    b->world = sim_world_create();
    if (!b->world) {
        free(b);
        sim_backend_set_error(error, DEVICE_ERR_NO_MEMORY, "sim_world 创建失败");
        return DEVICE_ERR_NO_MEMORY;
    }

    b->rng = sim_random_create(options->random_seed);
    if (!b->rng) {
        sim_world_destroy(b->world);
        free(b);
        sim_backend_set_error(error, DEVICE_ERR_NO_MEMORY, "sim_random 创建失败");
        return DEVICE_ERR_NO_MEMORY;
    }

    b->nvs = sim_nvs_open(b->nvs_file);
    if (!b->nvs) {
        sim_random_destroy(b->rng);
        sim_world_destroy(b->world);
        free(b);
        sim_backend_set_error(error, DEVICE_ERR_NO_MEMORY, "sim_nvs 创建失败");
        return DEVICE_ERR_NO_MEMORY;
    }

    if (sim_socket_startup() != DEMO_OK) {
        sim_nvs_close(b->nvs);
        sim_random_destroy(b->rng);
        sim_world_destroy(b->world);
        free(b);
        sim_backend_set_error(error, DEVICE_ERR_BACKEND_INIT, "WSAStartup 失败");
        return DEVICE_ERR_BACKEND_INIT;
    }
    b->wsa_ready = 1;

    sim_fault_init(&b->fault, b->world, b->tag,
                   b->sta_ssid, sizeof(b->sta_ssid), &b->sta_connected);

    out_instance->vtable = &g_sim_backend_vtable;
    out_instance->user = b;
    out_instance->destroy_user = sim_backend_destroy_user;
    return DEVICE_OK;
}
