#ifndef DEMO_DEVICE_DEVICE_EVENTS_H
#define DEMO_DEVICE_DEVICE_EVENTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 结构化集成事件写入器（--events-jsonl）。每行独立 JSON，写入后立即 flush。
 * 事件文件不可写时 device_events_open 返回 NULL。 */

typedef struct device_events device_events_t;

device_events_t *device_events_open(const char *path);
void device_events_close(device_events_t *e);

/* event：事件名；result：ok/fail；code：端侧错误码；data_json：可选附加字段 JSON 对象
 * （可为 NULL）。返回 DEMO_OK / DEMO_ERR。 */
int device_events_write(device_events_t *e, const char *event, const char *result,
                        int code, const char *data_json);

#ifdef __cplusplus
}
#endif

#endif
