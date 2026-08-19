#include "net_abstraction.h"
#include <stdlib.h>
#include <string.h>

struct net_ctx {
    const net_backend_t *backend;
    void *user;
    int initialized;
};

int net_ctx_create(const net_backend_t *be, void *user, const char *config_path,
                   net_ctx_t **out_ctx)
{
    if (out_ctx)
        *out_ctx = NULL;
    if (be == NULL || out_ctx == NULL)
        return DEMO_ERR_INVAL;
    net_ctx_t *ctx = (net_ctx_t *)malloc(sizeof(net_ctx_t));
    if (ctx == NULL)
        return DEMO_ERR_NOMEM;
    ctx->backend = be;
    ctx->user = user;
    ctx->initialized = 0;
    if (be->init) {
        int rc = be->init(user, config_path);
        if (rc != DEMO_OK) {
            free(ctx);
            return rc;
        }
        ctx->initialized = 1;
    }
    *out_ctx = ctx;
    return DEMO_OK;
}

void net_ctx_destroy(net_ctx_t *ctx)
{
    if (ctx == NULL)
        return;
    if (ctx->initialized && ctx->backend && ctx->backend->deinit)
        ctx->backend->deinit(ctx->user);
    free(ctx);
}

#define FWD0(func, member, ret_default) \
    int func(net_ctx_t *c) { \
        if (!c || !c->backend || !c->backend->member) return ret_default; \
        return c->backend->member(c->user); }

#define FWD1(func, member, ret_default) \
    int func(net_ctx_t *c, void *a) { \
        if (!c || !c->backend || !c->backend->member) return ret_default; \
        return c->backend->member(c->user, a); }

int net_wifi_scan(net_ctx_t *c, net_ap_info_t *aps, int *count)
{
    if (!c || !c->backend || !c->backend->wifi_scan || !aps || !count)
        return DEMO_ERR_INVAL;
    return c->backend->wifi_scan(c->user, aps, count);
}

int net_wifi_sta_connect(net_ctx_t *c, const char *ssid, const char *pass, wifi_reason_t *reason)
{
    if (!c || !c->backend || !c->backend->wifi_sta_connect)
        return DEMO_ERR_INVAL;
    return c->backend->wifi_sta_connect(c->user, ssid, pass, reason);
}

FWD0(net_wifi_sta_disconnect, wifi_sta_disconnect, DEMO_ERR)
FWD0(net_wifi_ap_stop, wifi_ap_stop, DEMO_ERR)

int net_wifi_ap_start(net_ctx_t *c, const char *ssid, const char *pass, const char *pin)
{
    if (!c || !c->backend || !c->backend->wifi_ap_start)
        return DEMO_ERR_INVAL;
    return c->backend->wifi_ap_start(c->user, ssid, pass, pin);
}

int net_wifi_ap_status(net_ctx_t *c, net_ap_status_t *status)
{
    if (!status)
        return DEMO_ERR_INVAL;
    memset(status, 0, sizeof(*status));
    if (!c || !c->backend || !c->backend->wifi_ap_status)
        return DEMO_ERR; /* 不支持平台/热点未运行：统一 DEMO_ERR（设计文档 7.1） */
    return c->backend->wifi_ap_status(c->user, status);
}

int net_wifi_ap_configure_ipv4(net_ctx_t *c, const char *ipv4, int prefix_length)
{
    if (!ipv4)
        return DEMO_ERR_INVAL;
    if (!c || !c->backend || !c->backend->wifi_ap_configure_ipv4)
        return DEMO_ERR;
    return c->backend->wifi_ap_configure_ipv4(c->user, ipv4, prefix_length);
}

int net_wifi_get_rssi(net_ctx_t *c, int *rssi)
{
    if (!c || !c->backend || !c->backend->wifi_get_rssi || !rssi)
        return DEMO_ERR_INVAL;
    return c->backend->wifi_get_rssi(c->user, rssi);
}

int net_wifi_get_ip(net_ctx_t *c, uint32_t *ip)
{
    if (!c || !c->backend || !c->backend->wifi_get_ip || !ip)
        return DEMO_ERR_INVAL;
    return c->backend->wifi_get_ip(c->user, ip);
}

int net_wifi_get_current_ssid(net_ctx_t *c, char *ssid, int capacity)
{
    if (!c || !c->backend || !c->backend->wifi_get_current_ssid || !ssid || capacity <= 0)
        return DEMO_ERR_INVAL;
    ssid[0] = '\0';
    return c->backend->wifi_get_current_ssid(c->user, ssid, capacity);
}

int net_wifi_get_gateway(net_ctx_t *c, uint32_t *ip)
{
    if (!ip)
        return DEMO_ERR_INVAL;
    *ip = 0;
    if (!c || !c->backend || !c->backend->wifi_get_gateway)
        return DEMO_ERR_INVAL;
    return c->backend->wifi_get_gateway(c->user, ip);
}

int net_tcp_listen(net_ctx_t *c, uint16_t port, void **sock)
{
    if (!c || !c->backend || !c->backend->tcp_listen || !sock)
        return DEMO_ERR_INVAL;
    return c->backend->tcp_listen(c->user, port, sock);
}

int net_tcp_accept(net_ctx_t *c, void *listen, void **conn, net_addr_t *peer)
{
    if (!c || !c->backend || !c->backend->tcp_accept || !conn)
        return DEMO_ERR_INVAL;
    return c->backend->tcp_accept(c->user, listen, conn, peer);
}

int net_tcp_connect(net_ctx_t *c, const net_addr_t *addr, void **sock, int timeout_ms)
{
    if (!c || !c->backend || !c->backend->tcp_connect || !addr || !sock)
        return DEMO_ERR_INVAL;
    return c->backend->tcp_connect(c->user, addr, sock, timeout_ms);
}

int net_sock_send(net_ctx_t *c, void *sock, const uint8_t *buf, int len)
{
    if (!c || !c->backend || !c->backend->sock_send || !sock || !buf)
        return DEMO_ERR_INVAL;
    return c->backend->sock_send(c->user, sock, buf, len);
}

int net_sock_recv(net_ctx_t *c, void *sock, uint8_t *buf, int cap)
{
    if (!c || !c->backend || !c->backend->sock_recv || !sock || !buf)
        return DEMO_ERR_INVAL;
    return c->backend->sock_recv(c->user, sock, buf, cap);
}

void net_sock_close(net_ctx_t *c, void *sock)
{
    if (!c || !c->backend || !c->backend->sock_close || !sock)
        return;
    c->backend->sock_close(c->user, sock);
}

int net_udp_mcast_join(net_ctx_t *c, const char *group, uint16_t port, void **sock)
{
    if (!c || !c->backend || !c->backend->udp_mcast_join || !sock)
        return DEMO_ERR_INVAL;
    return c->backend->udp_mcast_join(c->user, group, port, sock);
}

int net_udp_send(net_ctx_t *c, const char *group, uint16_t port, const uint8_t *buf, int len)
{
    if (!c || !c->backend || !c->backend->udp_send || !buf)
        return DEMO_ERR_INVAL;
    return c->backend->udp_send(c->user, group, port, buf, len);
}

int net_udp_recv(net_ctx_t *c, void *sock, uint8_t *buf, int cap, net_addr_t *from)
{
    if (!c || !c->backend || !c->backend->udp_recv || !sock || !buf)
        return DEMO_ERR_INVAL;
    return c->backend->udp_recv(c->user, sock, buf, cap, from);
}

int net_mdns_register(net_ctx_t *c, const net_mdns_service_t *svc)
{
    if (!c || !c->backend || !c->backend->mdns_register || !svc)
        return DEMO_ERR_INVAL;
    return c->backend->mdns_register(c->user, svc);
}

int net_mdns_unregister(net_ctx_t *c, const char *type)
{
    if (!c || !c->backend || !c->backend->mdns_unregister)
        return DEMO_ERR_INVAL;
    return c->backend->mdns_unregister(c->user, type);
}

int net_mdns_resolve(net_ctx_t *c, const char *type, net_mdns_service_t *out, int timeout_ms)
{
    if (!c || !c->backend || !c->backend->mdns_resolve || !out)
        return DEMO_ERR_INVAL;
    return c->backend->mdns_resolve(c->user, type, out, timeout_ms);
}

int net_nvs_get(net_ctx_t *c, const char *key, uint8_t *buf, int *len)
{
    if (!c || !c->backend || !c->backend->nvs_get || !key || !buf || !len)
        return DEMO_ERR_INVAL;
    return c->backend->nvs_get(c->user, key, buf, len);
}

int net_nvs_set(net_ctx_t *c, const char *key, const uint8_t *buf, int len)
{
    if (!c || !c->backend || !c->backend->nvs_set || !key || !buf)
        return DEMO_ERR_INVAL;
    return c->backend->nvs_set(c->user, key, buf, len);
}

int net_nvs_erase(net_ctx_t *c, const char *key)
{
    if (!c || !c->backend || !c->backend->nvs_erase || !key)
        return DEMO_ERR_INVAL;
    return c->backend->nvs_erase(c->user, key);
}

uint64_t net_time_ms(net_ctx_t *c)
{
    if (!c || !c->backend || !c->backend->time_ms)
        return 0;
    return c->backend->time_ms(c->user);
}

uint32_t net_random(net_ctx_t *c)
{
    if (!c || !c->backend || !c->backend->random)
        return 0;
    return c->backend->random(c->user);
}

int net_inject(net_ctx_t *c, const char *action, const char *arg_json)
{
    if (!c || !c->backend || !c->backend->inject || !action)
        return DEMO_ERR_INVAL;
    return c->backend->inject(c->user, action, arg_json);
}
