#ifndef DEMO_LINUX_NVS_H
#define DEMO_LINUX_NVS_H

/*
 * 真实 Linux 后端的文件 NVS。
 *
 * 存储格式与模拟后端一致：单个 JSON 文件（默认 run/device_linux.nvs.json），
 * 每键值为 base64 编码的 blob：
 *   { "wifi_creds": "<base64>", "host_candidates": "<base64>", "evlog": "<base64>" }
 * 写入使用同目录临时文件 + rename 原子替换。
 * 仅 Linux 下编译（依赖 POSIX 文件 API 与 cJSON）。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct linux_nvs linux_nvs_t;

/* 创建文件 NVS 实例。path 为空时使用默认 run/device_linux.nvs.json。
 * 只校验路径格式，不创建文件（首次写入时创建父目录）。 */
int linux_nvs_create(linux_nvs_t **out, const char *path);

void linux_nvs_destroy(linux_nvs_t *nvs);

/* len 入=缓冲区容量 出=实际长度；键不存在返回 DEMO_ERR 且 *len=0。 */
int linux_nvs_get(linux_nvs_t *nvs, const char *key, uint8_t *buf, int *len);

int linux_nvs_set(linux_nvs_t *nvs, const char *key, const uint8_t *buf, int len);

int linux_nvs_erase(linux_nvs_t *nvs, const char *key);

#ifdef __cplusplus
}
#endif

#endif
