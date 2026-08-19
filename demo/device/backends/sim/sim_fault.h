/* ============================================================================
 * sim_fault.h - 故障注入（纯 C11，显式实例）。
 *
 * 保持旧 net_sim/sim_backend.cpp Inject() 的全部既有 action 语义：
 *   wifi_disconnect / wifi_ok / wifi_auth_fail / wifi_auth_ok /
 *   wifi_ssid_mismatch / rssi_set / mcast_block / mcast_unblock /
 *   burst_send / sock_send_fail；
 * 未知 action 返回 DEMO_ERR。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_FAULT_H
#define DEMO_DEVICE_SIM_FAULT_H

#include "sim_world.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_fault {
    sim_world_t *world;
    const char *tag;
    char *sta_ssid;
    size_t sta_ssid_cap;
    int *sta_connected;
    int send_fail_skip;
    int send_fail_count;
} sim_fault_t;

/* sta_ssid/sta_connected 指向所属后端的关联 SSID 状态（wifi_ssid_mismatch 使用） */
void sim_fault_init(sim_fault_t *f, sim_world_t *world, const char *tag,
                    char *sta_ssid, size_t sta_ssid_cap, int *sta_connected);

/* 返回 1=本次发送被强制失败（sock_send_fail 注入），并消耗一次计数 */
int sim_fault_should_fail_send(sim_fault_t *f);

/* 执行注入；DEMO_OK=成功，DEMO_ERR=未知 action 或前置条件不满足 */
int sim_fault_inject(sim_fault_t *f, const char *action, const char *arg_json);

#ifdef __cplusplus
}
#endif

#endif
