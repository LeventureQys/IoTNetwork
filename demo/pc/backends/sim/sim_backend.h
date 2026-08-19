#ifndef PC_SIM_BACKEND_H
#define PC_SIM_BACKEND_H

#include "net_abstraction.h"
#include "params.h"

typedef struct sim_ap_record {
    char ssid[33];
    char password[64];
    char pin[8];
    char device_id[18];
    uint16_t real_port;
    char logical_ip[16];
    char loopback_host[16];
} sim_ap_record_t;

#ifdef __cplusplus
extern "C" {
#endif

/* 创建 PC sim 后端实例：tag 仅用于日志标识与 in-process 模拟（"host" 或 "dev<n>"）。
 * 仅做 PC 模拟：WiFi scan/connect、AP catalog 读取、endpoint 翻译；
 * 不含 Linux 热点、设备 NVS、设备状态（设计文档第 7.1/8.5 节）。 */
void *sim_backend_create(const char *tag, const demo_params_t *params);
void  sim_backend_destroy(void *user);
const net_backend_t *sim_backend_table(void);

/* 跨进程模拟热点目录读取（设备进程按 device-<index>.json 契约发布，PC 只读不删）。
 * 每次调用重新扫描目录；跳过 .tmp-* 文件；>30 秒且 owner_pid 不存在视为过期。 */
int sim_backend_ap_find(const demo_params_t *params, const char *ssid,
                        sim_ap_record_t *out);
int sim_backend_ap_list(const demo_params_t *params, sim_ap_record_t *out,
                        int capacity);

#ifdef __cplusplus
}
#endif

#endif
