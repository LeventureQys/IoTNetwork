/* ============================================================================
 * sim_world.h - 模拟网络世界的显式实例状态（纯 C11）。
 *
 * 对应旧 net_sim/sim_world.cpp 的实例化重写：同一进程内每个后端持有
 * 独立的 sim_world_t 实例（"两实例隔离"）。线程安全：所有接口内部加锁
 * （Windows SRWLock / POSIX pthread_mutex），语义与旧单例一致。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_WORLD_H
#define DEMO_DEVICE_SIM_WORLD_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_WORLD_MAX_APS     16
#define SIM_WORLD_MAX_RSSI    17
#define SIM_WORLD_MAX_MDNS    4
#define SIM_WORLD_MAX_BLOCKED 17

typedef struct sim_ap {
    char ssid[33];
    char password[64];
    char pin[8];
    uint16_t real_port;
    char owner_tag[16];
    int active;
} sim_ap_t;

typedef struct sim_mdns_svc {
    char instance[64];
    char type[64];
    uint32_t ip;       /* 网络字节序 */
    uint16_t port;     /* 网络字节序 */
    char txt[128];
    int active;
} sim_mdns_svc_t;

typedef struct sim_world sim_world_t;

sim_world_t *sim_world_create(void);
void sim_world_destroy(sim_world_t *w);

/* 目标网络（启动时注册一次） */
void sim_world_target_set(sim_world_t *w, const char *ssid, const char *pass, int band_2g);
void sim_world_target_set_up(sim_world_t *w, int up);
int sim_world_target_up(const sim_world_t *w);
void sim_world_target_set_auth_fail(sim_world_t *w, int on);
int sim_world_target_auth_fail(const sim_world_t *w);
const char *sim_world_target_ssid(const sim_world_t *w);
const char *sim_world_target_password(const sim_world_t *w);
int sim_world_target_band_2g(const sim_world_t *w);

/* 虚拟 AP */
void sim_world_ap_register(sim_world_t *w, const char *owner_tag, const char *ssid,
                           const char *pass, const char *pin, uint16_t real_port);
void sim_world_ap_unregister(sim_world_t *w, const char *owner_tag);
int sim_world_ap_find(const sim_world_t *w, const char *ssid, sim_ap_t *out);
int sim_world_ap_count(const sim_world_t *w);
int sim_world_ap_iterate(const sim_world_t *w, int *pos, sim_ap_t *out); /* pos 0 起，找到返回 1 并前进 */

/* STA 连接判定：返回 0=ok，否则 wifi_reason_t（见 protocol.h） */
int sim_world_sta_connect(const sim_world_t *w, const char *ssid, const char *pass);

/* RSSI（按 tag） */
void sim_world_rssi_set(sim_world_t *w, const char *tag, int rssi);
int sim_world_rssi_get(const sim_world_t *w, const char *tag);

/* mDNS 注册表 */
void sim_world_mdns_register(sim_world_t *w, const sim_mdns_svc_t *svc);
void sim_world_mdns_unregister(sim_world_t *w, const char *type);
int sim_world_mdns_resolve(const sim_world_t *w, const char *type, sim_mdns_svc_t *out);

/* 组播屏蔽（按 tag） */
void sim_world_mcast_set_blocked(sim_world_t *w, const char *tag, int blocked);
int sim_world_mcast_is_blocked(const sim_world_t *w, const char *tag);

/* 虚拟 IP（网络字节序），beta v1.1 角色反转语义：
 * PC 是热点网关 192.168.137.1；设备 STA 位于 192.168.137.100+。 */
uint32_t sim_world_pc_ap_ip(void);
uint32_t sim_world_device_sta_virtual_ip(int dev_index);

#ifdef __cplusplus
}
#endif

#endif
