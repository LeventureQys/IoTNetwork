/* ============================================================================
 * sim_nvs.h - JSON blob 键值存储（纯 C11，实例化）。
 *
 * 文件格式与旧模拟 NVS 兼容：<file> = JSON 对象，值为 base64 编码的 blob。
 * 行为契约（相对旧实现的两处收紧）：
 *   1. 非法 base64 一律失败（旧实现静默跳过非法字符）；
 *   2. 缓冲不足时失败并报告所需长度（*out_len = needed），绝不截断。
 * 写入采用同目录 tmp 文件 + flush + 原子替换；失败清理 tmp。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_NVS_H
#define DEMO_DEVICE_SIM_NVS_H

#include <stdint.h>
#include <stddef.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sim_nvs sim_nvs_t;

/* 打开 NVS 存储（惰性：不触碰磁盘，读取在每次 get/set 时发生）。
 * 返回 NULL 仅表示内存不足。 */
sim_nvs_t *sim_nvs_open(const char *file_path);
void sim_nvs_close(sim_nvs_t *nvs);

/* len 入=容量（cap），出=实际长度；键不存在或文件损坏返回 DEMO_ERR 且 *len=0。
 * 实际长度 > cap 时返回 DEMO_ERR，*len 报告所需长度（不截断）。 */
int sim_nvs_get(const sim_nvs_t *nvs, const char *key, uint8_t *buf, int cap, int *out_len);

/* 写入/覆盖键；文件损坏时重建空对象（与旧实现一致）。失败返回 DEMO_ERR。 */
int sim_nvs_set(sim_nvs_t *nvs, const char *key, const uint8_t *buf, int len);
int sim_nvs_erase(sim_nvs_t *nvs, const char *key);

/* base64：严格模式。返回 DEMO_OK / DEMO_ERR（非法输入）/
 * DEMO_ERR（输出不足，*out_len 报所需长度）。 */
int sim_nvs_base64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap, size_t *out_len);
int sim_nvs_base64_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
