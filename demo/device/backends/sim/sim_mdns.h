/* ============================================================================
 * sim_mdns.h - 模拟 mDNS 注册/解析（纯 C11，无全局状态）。
 *
 * 语义冻结于旧 net_sim/sim_backend.cpp 的 mDNS 部分：注册表状态存放于
 * sim_world 实例；resolve 以 10ms 周期轮询至超时（timeout<=0 时默认 2000ms）。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_MDNS_H
#define DEMO_DEVICE_SIM_MDNS_H

#include "net_abstraction.h"
#include "sim_world.h"

#ifdef __cplusplus
extern "C" {
#endif

int sim_mdns_register(sim_world_t *world, const net_mdns_service_t *svc);
int sim_mdns_unregister(sim_world_t *world, const char *type);
int sim_mdns_resolve(sim_world_t *world, const char *type, net_mdns_service_t *out,
                     int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
