#ifndef DEMO_DEVICE_DEVICE_CMD_QUEUE_H
#define DEMO_DEVICE_DEVICE_CMD_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 固定容量 32 的 C 命令环形队列：满时 BUSY 不覆盖；单生产者多消费者安全。 */

#define DEVICE_CMD_QUEUE_CAPACITY 32

typedef enum device_cmd_kind {
    DEVICE_CMD_APP_DATA = 0,
    DEVICE_CMD_SERIAL_BYTES,
    DEVICE_CMD_INJECT_FAULT
} device_cmd_kind_t;

typedef struct device_cmd {
    device_cmd_kind_t kind;
    char app_data[APP_DATA_TEXT_MAX];
    size_t app_data_len;
    char fault_action[64];
    char fault_argument[512];
    /* 二进制串口 chunk。所有权随 push 转移给队列，随 pop 转移给消费者；
     * 消费者必须 free。仅 DEVICE_CMD_SERIAL_BYTES 使用。 */
    uint8_t *serial_data;
    size_t serial_len;
} device_cmd_t;

typedef struct device_mutex device_mutex_t;
typedef struct device_cond device_cond_t;
typedef struct device_platform device_platform_t;

typedef struct device_cmd_queue device_cmd_queue_t;

#define DEVICE_QUEUE_OK    0
#define DEVICE_QUEUE_EMPTY 1
#define DEVICE_QUEUE_FULL  2

device_cmd_queue_t *device_cmd_queue_create(const device_platform_t *plat);
void device_cmd_queue_destroy(device_cmd_queue_t *q);
int device_cmd_queue_push(device_cmd_queue_t *q, const device_cmd_t *cmd);
int device_cmd_queue_pop(device_cmd_queue_t *q, device_cmd_t *out);
int device_cmd_queue_count(device_cmd_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif
