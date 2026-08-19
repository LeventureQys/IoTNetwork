#include "device_cmd_queue.h"
#include "device_platform.h"
#include <stdlib.h>
#include <string.h>

struct device_cmd_queue {
    const device_platform_t *plat;
    device_mutex_t *mutex;
    device_cmd_t slots[DEVICE_CMD_QUEUE_CAPACITY];
    size_t head;
    size_t count;
};

device_cmd_queue_t *device_cmd_queue_create(const device_platform_t *plat)
{
    if (plat == NULL)
        return NULL;
    device_cmd_queue_t *q = (device_cmd_queue_t *)calloc(1, sizeof(device_cmd_queue_t));
    if (q == NULL)
        return NULL;
    q->plat = plat;
    q->mutex = plat->mutex_create();
    if (q->mutex == NULL) {
        free(q);
        return NULL;
    }
    return q;
}

void device_cmd_queue_destroy(device_cmd_queue_t *q)
{
    if (q == NULL)
        return;
    q->plat->mutex_destroy(q->mutex);
    free(q);
}

int device_cmd_queue_push(device_cmd_queue_t *q, const device_cmd_t *cmd)
{
    if (q == NULL || cmd == NULL)
        return DEVICE_QUEUE_FULL;
    q->plat->mutex_lock(q->mutex);
    int rc = DEVICE_QUEUE_OK;
    if (q->count >= DEVICE_CMD_QUEUE_CAPACITY) {
        rc = DEVICE_QUEUE_FULL;
    } else {
        size_t idx = (q->head + q->count) % DEVICE_CMD_QUEUE_CAPACITY;
        q->slots[idx] = *cmd;
        q->count++;
    }
    q->plat->mutex_unlock(q->mutex);
    return rc;
}

int device_cmd_queue_pop(device_cmd_queue_t *q, device_cmd_t *out)
{
    if (q == NULL || out == NULL)
        return DEVICE_QUEUE_EMPTY;
    q->plat->mutex_lock(q->mutex);
    int rc = DEVICE_QUEUE_OK;
    if (q->count == 0) {
        rc = DEVICE_QUEUE_EMPTY;
    } else {
        *out = q->slots[q->head];
        q->head = (q->head + 1) % DEVICE_CMD_QUEUE_CAPACITY;
        q->count--;
    }
    q->plat->mutex_unlock(q->mutex);
    return rc;
}

int device_cmd_queue_count(device_cmd_queue_t *q)
{
    if (q == NULL)
        return 0;
    q->plat->mutex_lock(q->mutex);
    int n = (int)q->count;
    q->plat->mutex_unlock(q->mutex);
    return n;
}
