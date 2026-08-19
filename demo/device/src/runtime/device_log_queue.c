#include "device_log_queue.h"
#include "device_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct device_log_queue {
    const device_platform_t *plat;
    device_mutex_t *mutex;
    device_log_record_t slots[DEVICE_LOG_QUEUE_CAPACITY];
    size_t head;
    size_t count;
    uint64_t sequence;
    uint64_t dropped;
};

device_log_queue_t *device_log_queue_create(const device_platform_t *plat)
{
    if (plat == NULL)
        return NULL;
    device_log_queue_t *q = (device_log_queue_t *)calloc(1, sizeof(device_log_queue_t));
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

void device_log_queue_destroy(device_log_queue_t *q)
{
    if (q == NULL)
        return;
    q->plat->mutex_destroy(q->mutex);
    free(q);
}

void device_log_queue_push(device_log_queue_t *q, int level, const char *module,
                           const char *message)
{
    if (q == NULL)
        return;
    q->plat->mutex_lock(q->mutex);
    q->sequence++;
    device_log_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.sequence = q->sequence;
    rec.timestamp_ms = q->plat->monotonic_ms();
    rec.level = level;
    if (module != NULL)
        snprintf(rec.module, sizeof(rec.module), "%s", module);
    if (message != NULL)
        snprintf(rec.message, sizeof(rec.message), "%s", message);
    if (q->count >= DEVICE_LOG_QUEUE_CAPACITY) {
        q->dropped++;
        q->slots[q->head] = rec; /* 覆盖最旧 */
        q->head = (q->head + 1) % DEVICE_LOG_QUEUE_CAPACITY;
    } else {
        size_t idx = (q->head + q->count) % DEVICE_LOG_QUEUE_CAPACITY;
        q->slots[idx] = rec;
        q->count++;
    }
    q->plat->mutex_unlock(q->mutex);
}

int device_log_queue_pull(device_log_queue_t *q, device_log_record_t *records,
                          size_t capacity, size_t *out_count, uint64_t *out_dropped)
{
    if (q == NULL || out_count == NULL || out_dropped == NULL)
        return DEMO_ERR_INVAL;
    *out_count = 0;
    *out_dropped = 0;
    if (capacity > 0 && records == NULL)
        return DEMO_ERR_INVAL;
    q->plat->mutex_lock(q->mutex);
    size_t n = capacity < q->count ? capacity : q->count;
    for (size_t i = 0; i < n; i++) {
        records[i] = q->slots[(q->head + i) % DEVICE_LOG_QUEUE_CAPACITY];
    }
    q->head = (q->head + n) % DEVICE_LOG_QUEUE_CAPACITY;
    q->count -= n;
    *out_count = n;
    *out_dropped = q->dropped;
    q->plat->mutex_unlock(q->mutex);
    return DEVICE_OK;
}
