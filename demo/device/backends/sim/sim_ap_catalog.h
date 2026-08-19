/* ============================================================================
 * sim_ap_catalog.h - 跨进程 PC 热点 catalog（纯 C11，无全局状态）。
 *
 * beta v1.1 角色反转（设计文档 7.3 / 阶段二 DQ6）：
 *   - PC 是唯一发布者；设备只读取，不发布、不删除 PC 记录。
 *   - 文件名：<dir>/pc-hotspot.json（单一文件，非 device-<index>.json 多文件）。
 *   - JSON schema（schema=2）字段全部必需且类型严格：
 *     schema、ssid、password、logical_gateway、prefix_length、tcp_port、
 *     loopback_host、loopback_port、published_at_ms、owner_pid；
 *     未知字段忽略。
 *   - 读取校验：schema 非 2、字段缺失/类型错误、IP 非法、端口越界、
 *     prefix_length 越界或 JSON 损坏 → 记录忽略（fail-closed）。
 *   - 时效：记录超过 30 秒且 owner_pid 已不存在 → 忽略；
 *     无法判断 PID 是否存在时仅使用 30 秒时效。
 *   - 发布端的 tmp+flush+原子 rename 由 PC 端保证；本模块读取端通过
 *     sim_util_read_file 的共享删除打开标志并发安全地读取正式文件。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_AP_CATALOG_H
#define DEMO_DEVICE_SIM_AP_CATALOG_H

#include <stdint.h>
#include <stddef.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_AP_CATALOG_SCHEMA       2
#define SIM_AP_CATALOG_TTL_MS       30000
#define SIM_AP_CATALOG_MAX_FILE     8192

typedef struct sim_ap_catalog_record {
    int schema;
    char ssid[33];
    char password[64];
    char logical_gateway[16];
    int prefix_length;
    unsigned int tcp_port;
    char loopback_host[32];
    unsigned int loopback_port;
    uint64_t published_at_ms;
    unsigned long owner_pid;
} sim_ap_catalog_record_t;

/* 墙钟（毫秒，跨进程比较用；published_at_ms 采用此时钟） */
uint64_t sim_ap_catalog_wallclock_ms(void);

/* 当前进程 PID（墙钟语义，跨进程契约/测试构造 owner_pid 用） */
unsigned long sim_ap_catalog_current_pid(void);

/* 正式文件名 <dir>/pc-hotspot.json（绝对路径拼接）；返回 DEMO_OK / DEMO_ERR_INVAL */
int sim_ap_catalog_build_path(const char *dir, char *out, size_t out_cap);

/* 读取 PC 热点记录并做字段校验 + 时效过滤。
 * now_ms==0 时使用墙钟；缺失/损坏/字段非法/过期(owner 不存活) → DEMO_ERR，
 * 成功填充 out。 */
int sim_ap_catalog_read(const char *dir, sim_ap_catalog_record_t *out, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
