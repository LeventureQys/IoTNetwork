#ifndef DEMO_DEVICE_DEVICE_LOG_QUEUE_H
#define DEMO_DEVICE_DEVICE_LOG_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include "common.h"
#include "device_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 日志 C 队列（pull 模式）：push 永不失败，溢出累计 dropped。 */

#define DEVICE_LOG_QUEUE_CAPACITY 1024

typedef struct device_mutex device_mutex_t;
typedef struct device_platform device_platform_t;
typedef struct device_log_queue device_log_queue_t;

device_log_queue_t *device_log_queue_create(const device_platform_t *plat);
void device_log_queue_destroy(device_log_queue_t *q);
void device_log_queue_push(device_log_queue_t *q, int level, const char *module,
                           const char *message);
/* 拉取最多 capacity 条到 records；out_count=实际条数；out_dropped=累计丢弃数。 */
int device_log_queue_pull(device_log_queue_t *q, device_log_record_t *records,
                          size_t capacity, size_t *out_count, uint64_t *out_dropped);

#ifdef __cplusplus
}
#endif

#endif
