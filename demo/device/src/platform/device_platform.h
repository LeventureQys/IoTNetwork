#ifndef DEMO_DEVICE_DEVICE_PLATFORM_H
#define DEMO_DEVICE_DEVICE_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 纯 C 平台抽象：线程（带超时 join）、互斥、条件变量、单调时钟。
 * 类型不透明；全部对象通过 *_create 分配、*_destroy 释放。 */

typedef struct device_thread device_thread_t;
typedef struct device_mutex device_mutex_t;
typedef struct device_cond device_cond_t;

#define DEVICE_PLAT_OK       0
#define DEVICE_PLAT_TIMEOUT  1

typedef struct device_platform {
    device_thread_t *(*thread_create)(void (*entry)(void *user), void *user);
    int  (*thread_join)(device_thread_t *thread, uint32_t timeout_ms);
    void (*thread_destroy)(device_thread_t *thread);

    device_mutex_t *(*mutex_create)(void);
    void (*mutex_destroy)(device_mutex_t *mutex);
    void (*mutex_lock)(device_mutex_t *mutex);
    void (*mutex_unlock)(device_mutex_t *mutex);

    device_cond_t *(*cond_create)(void);
    void (*cond_destroy)(device_cond_t *cond);
    void (*cond_wait)(device_cond_t *cond, device_mutex_t *mutex);
    int  (*cond_wait_timeout)(device_cond_t *cond, device_mutex_t *mutex,
                              uint32_t timeout_ms);
    void (*cond_signal)(device_cond_t *cond);
    void (*cond_broadcast)(device_cond_t *cond);

    uint64_t (*monotonic_ms)(void);
} device_platform_t;

extern const device_platform_t device_platform_default;

#ifdef __cplusplus
}
#endif

#endif
