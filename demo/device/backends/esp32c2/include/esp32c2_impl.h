#ifndef DEMO_ESP32C2_IMPL_H
#define DEMO_ESP32C2_IMPL_H

#include "net_abstraction.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32-C2 后端实例数据（真实移植时扩展） */
typedef struct esp32c2_ctx {
    /* 占位：真实实现持有 netif/wifi 句柄等 */
    int placeholder;
} esp32c2_ctx_t;

/* 获取 esp32c2 后端 vtable（骨架，未在真实硬件验证） */
const net_backend_t *esp32c2_backend_get(void);

#ifdef __cplusplus
}
#endif

#endif
