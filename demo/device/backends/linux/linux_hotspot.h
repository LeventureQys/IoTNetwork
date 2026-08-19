#ifndef DEMO_LINUX_HOTSPOT_H
#define DEMO_LINUX_HOTSPOT_H

/*
 * 真实 Linux 热点（hostapd + dnsmasq）模块，实例化版本。
 *
 * 在现有 STA 上网卡不中断的前提下，于同一 phy 创建虚拟 AP 接口并启动
 * hostapd/dnsmasq/NAT，使"设备配网热点"在真实硬件上可见。仅 Linux 下
 * 编译与生效。
 *
 * 语义（与旧版进程级实现一致，全部改为实例）：
 * - 保留热点地址规划（linux_hotspot_config.h）；
 * - 保留单实例锁（work_dir 下 modu_linux_hotspot.lock，flock）；
 * - 保留 readiness 检查与 fail-closed；失败时按逆序 rollback；
 * - 外部命令全部走 argv + fork/exec（linux_exec.h），不拼 shell 字符串。
 *
 * 注意：创建虚拟接口、启动 hostapd 需要 root 权限；非 root 时 create 失败
 * （调用方不得回退模拟后端）。
 */

#include "common.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINUX_HOTSPOT_DEFAULT_CONFIG "config/linux_hotspot.json"

/* 前向声明（完整定义见 linux_hotspot_linux_ops.h；本头不得包含该文件，
 * 否则与 linux_hotspot_config.h 的包含链构成环）。 */
typedef struct linux_hotspot_ops linux_hotspot_ops_t;

typedef struct linux_hotspot_cfg {
    int  enable;                        /* 总开关 */
    char sta_interface[32];             /* 现有 STA 网卡（如 wlP2p33s0） */
    char ap_interface[32];              /* 虚拟 AP 接口名（如 ap0） */
    char subnet[32];                    /* AP 网关地址（如 10.42.0.1） */
    int  prefix_length;                 /* AP 子网前缀长度 */
    char dhcp_start[32];                /* DHCP 池起始（如 10.42.0.100） */
    char dhcp_end[32];                  /* DHCP 池结束（如 10.42.0.200） */
    int  channel;                       /* 0 = 自动（跟随 STA 当前信道） */
    int  nat;                           /* 是否给热点客户端做 SNAT 共享上网 */
    char hostapd_bin[64];
    char dnsmasq_bin[64];
    char work_dir[128];                 /* hostapd 配置 / pid 文件目录 */
} linux_hotspot_cfg_t;

typedef struct linux_hotspot linux_hotspot_t;

/*
 * 创建热点实例：
 * - config_path 为 NULL/空时使用 LINUX_HOTSPOT_DEFAULT_CONFIG；
 * - ops_override 非 NULL 时以该 ops 表驱动（仅供测试注入，实例生命周期内
 *   调用者必须保证 ops 表存活）；NULL 使用生产实现；
 * - sta_interface_override 非空时覆盖配置中的 STA 网卡名（加载后重新校验）；
 * - 流程：读取并严格校验配置 -> root 检查 -> 单实例锁 -> 就绪。
 * 失败时 *out 置 NULL，不残留任何锁或文件。错误信息写入 error。
 */
int linux_hotspot_create(linux_hotspot_t **out, const char *config_path,
                         const linux_hotspot_ops_t *ops_override,
                         const char *sta_interface_override,
                         char *error, size_t error_capacity);

/* 幂等销毁：先停止热点并逆序 rollback，再释放单实例锁并释放实例。 */
void linux_hotspot_destroy(linux_hotspot_t *hotspot);

/* 幂等：已启动则先停止再以新 SSID/密码启动。非 root 或配置关闭时返回 DEMO_ERR */
int linux_hotspot_start(linux_hotspot_t *hotspot, const char *ssid,
                        const char *password);

/* 幂等：停止 hostapd/dnsmasq，删除虚拟 AP 接口 */
int linux_hotspot_stop(linux_hotspot_t *hotspot);

int linux_hotspot_is_active(const linux_hotspot_t *hotspot);

const char *linux_hotspot_sta_interface(const linux_hotspot_t *hotspot);

#ifdef __cplusplus
}
#endif

#endif
