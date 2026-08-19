#ifndef PC_SIM_BACKEND_H
#define PC_SIM_BACKEND_H

#include "net_abstraction.h"
#include "params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PC sim 后端：发布 schema2 pc-hotspot.json（设计文档 7.3）、TCP loopback、
 * 时钟/随机数及故障注入。tag 仅用于日志标识；PC 侧使用 "host"。
 * 设备侧 sim 由 demo/device/backends/sim 独立实现，两端只通过 catalog 文件交互。 */
void *sim_backend_create(const char *tag, const demo_params_t *params);
void  sim_backend_destroy(void *user);
const net_backend_t *sim_backend_table(void);

#ifdef __cplusplus
}
#endif

#endif
