/* ============================================================================
 * sim_ap_catalog.h - 跨进程模拟 AP catalog（纯 C11，无全局状态）。
 *
 * 契约冻结于设计文档第 8.5 节（本模块与 PC 端 SS02 共同实现）：
 *   - 文件名：device-<device_index>.json（索引十进制 0～15）。
 *   - 写入：同目录 <name>.tmp-<pid> → flush → 原子替换正式文件。
 *   - 删除：停止 AP / 正常退出时删除正式文件及本进程 tmp。
 *   - JSON schema（schema=1）字段全部必需且类型严格：
 *     schema、device_index、device_id、ssid、bssid、logical_ip、
 *     loopback_host、provision_port、published_at_ms、owner_pid；
 *     未知字段忽略。
 *   - 读取校验：schema 非 1、索引/端口越界、IP 非法或 JSON 损坏 → 忽略。
 *   - 时效：记录超过 30 秒且 owner_pid 已不存在 → 忽略；
 *     无法判断 PID 是否存在时仅使用 30 秒时效。
 *   - PC 不删除设备正式文件；本模块的 remove 仅用于设备侧停止 AP/退出。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_AP_CATALOG_H
#define DEMO_DEVICE_SIM_AP_CATALOG_H

#include <stdint.h>
#include <stddef.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_AP_CATALOG_SCHEMA       1
#define SIM_AP_CATALOG_MAX_INDEX    15
#define SIM_AP_CATALOG_TTL_MS       30000
#define SIM_AP_CATALOG_MAX_FILE     8192

typedef struct sim_ap_catalog_record {
    int schema;
    unsigned int device_index;
    char device_id[18];
    char ssid[33];
    char bssid[18];
    char logical_ip[16];
    char loopback_host[32];
    unsigned int provision_port;
    uint64_t published_at_ms;
    unsigned long owner_pid;
} sim_ap_catalog_record_t;

/* 当前进程 PID（墙钟语义，跨进程契约用） */
unsigned long sim_ap_catalog_current_pid(void);
uint64_t sim_ap_catalog_wallclock_ms(void);

/* 按冻结格式推导设备标识：02:00:00:00:00:%02X（index+1，与旧 main 一致） */
void sim_ap_catalog_build_device_id(unsigned int device_index, char *out, size_t cap);

/* 正式文件名 device-<index>.json（绝对路径拼接）；返回 0=成功 */
int sim_ap_catalog_build_path(const char *dir, unsigned int device_index,
                              char *out, size_t out_cap);

/* 发布：目录不存在则创建；tmp+flush+原子替换；失败清理 tmp 并返回 DEMO_ERR */
int sim_ap_catalog_publish(const char *dir, const sim_ap_catalog_record_t *record);

/* 删除正式文件及本进程 tmp（<name>.tmp-<pid>）；不存在视为成功 */
int sim_ap_catalog_remove(const char *dir, unsigned int device_index, unsigned long pid);

/* 读取单个索引的正式文件；未找到/校验失败返回 DEMO_ERR */
int sim_ap_catalog_read(const char *dir, unsigned int device_index,
                        sim_ap_catalog_record_t *out);

/* 扫描目录：仅正式文件（跳过 .tmp-*），字段校验 + 时效过滤。
 * now_ms==0 时使用墙钟；out_count 输出有效记录数（可能超过 capacity，不计越界填充）。 */
int sim_ap_catalog_list(const char *dir, sim_ap_catalog_record_t *out, int capacity,
                        int *out_count, uint64_t now_ms);

/* 按 SSID 查找（内部 list + 线性扫描） */
int sim_ap_catalog_find_ssid(const char *dir, const char *ssid,
                             sim_ap_catalog_record_t *out, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
