#ifndef PC_EVENT_H
#define PC_EVENT_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 结构化集成事件（设计文档第 11 节）：JSONL 每行一条，字段
 * schema/seq/time_ms/role/event/result/code/data（设备相关事件另含 device_id）。
 * 仅当调用方显式指定 --events-jsonl 时创建通道；未打开时 pc_events_emit 为空操作。 */

typedef struct pc_event {
    const char *event;     /* ready/auth_result/...（见设计文档第 11 节事件表） */
    const char *result;    /* "ok" | "fail" */
    int code;              /* 稳定端侧错误码或底层 DEMO_* 映射 */
    const char *device_id; /* 设备相关事件填设备 ID，否则 NULL/空串 */
    const char *data_json; /* 合法 JSON 对象字符串；NULL/空串 → {} */
} pc_event_t;

typedef void (*pc_events_observer_fn)(const char *event, uint64_t time_ms);

/* 打开写入器：文件不可写（含目录不存在）返回错误；打开失败不产生任何输出。 */
int pc_events_open(const char *path);
void pc_events_close(void);              /* 幂等：flush 并关闭 */
int pc_events_enabled(void);

/* 线程安全；未打开时为空操作。seq 从 1 单调递增，每行完整 flush。 */
void pc_events_emit(const pc_event_t *ev);

/* 观察者：每条事件（无论通道是否打开）都会回调，用于 scenario 的 after_event 门控。
 * 单订阅者，置 NULL 注销。回调在调用线程同步执行，必须轻量且线程安全。 */
void pc_events_set_observer(pc_events_observer_fn observer);

/* 是否已观测到指定事件名（事件名精确匹配，供 scenario 门控轮询） */
int pc_events_was_seen(const char *event);

/* 进程内统一单调毫秒时钟（基准在首次调用时固定，仅相对比较有意义）。
 * scenario 门控的到达时刻与等待均使用该基准，保证毫秒级精度。 */
uint64_t pc_events_now_ms(void);

/* 事件首次到达的单调毫秒时刻（与 pc_events_now_ms 同基准，事件名精确匹配）；
 * 从未观测到返回 0。供 scenario 将 delay_ms 锚定到事件到达时刻。 */
uint64_t pc_events_last_seen_ms(const char *event);

#ifdef __cplusplus
}
#endif

#endif
