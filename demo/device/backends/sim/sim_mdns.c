/* ============================================================================
 * sim_mdns.c - 模拟 mDNS 注册/解析（纯 C11）。
 *
 * 迁移自旧 net_sim/sim_backend.cpp 的 mDNS 部分；注册表状态在 sim_world
 * 实例内，resolve 轮询循环与超时语义逐字保持。
 * ========================================================================== */
#include "sim_mdns.h"
#include "sim_util.h"
#include "common.h"

#include <string.h>

int sim_mdns_register(sim_world_t *world, const net_mdns_service_t *svc)
{
    if (!world || !svc)
        return DEMO_ERR_INVAL;
    sim_mdns_svc_t s;
    memset(&s, 0, sizeof(s));
    sim_util_copy_bounded(s.instance, sizeof(s.instance), svc->instance);
    sim_util_copy_bounded(s.type, sizeof(s.type), svc->type);
    s.ip = svc->addr.ip;
    s.port = svc->addr.port;
    sim_util_copy_bounded(s.txt, sizeof(s.txt), svc->txt);
    s.active = 1;
    sim_world_mdns_register(world, &s);
    return DEMO_OK;
}

int sim_mdns_unregister(sim_world_t *world, const char *type)
{
    if (!world || !type)
        return DEMO_ERR_INVAL;
    sim_world_mdns_unregister(world, type);
    return DEMO_OK;
}

int sim_mdns_resolve(sim_world_t *world, const char *type, net_mdns_service_t *out,
                     int timeout_ms)
{
    if (!world || !type || !out)
        return DEMO_ERR_INVAL;
    uint64_t deadline = sim_util_monotonic_ms() +
                        (uint64_t)(timeout_ms <= 0 ? 2000 : timeout_ms);
    for (;;) {
        sim_mdns_svc_t s;
        if (sim_world_mdns_resolve(world, type, &s)) {
            memset(out, 0, sizeof(*out));
            sim_util_copy_bounded(out->instance, sizeof(out->instance), s.instance);
            sim_util_copy_bounded(out->type, sizeof(out->type), s.type);
            out->addr.ip = s.ip;
            out->addr.port = s.port;
            sim_util_copy_bounded(out->txt, sizeof(out->txt), s.txt);
            return DEMO_OK;
        }
        if (sim_util_monotonic_ms() >= deadline)
            break;
        sim_util_sleep_ms(10);
    }
    return DEMO_ERR_TIMEOUT;
}
