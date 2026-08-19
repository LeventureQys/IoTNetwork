#include "pc_event.h"
#include "cJSON.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_ev_lock;
static int g_ev_lock_init = 0;
#else
#include <pthread.h>
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static FILE *g_ev_file = NULL;
static uint64_t g_ev_seq = 0;
static volatile pc_events_observer_fn g_ev_observer = NULL;
static struct SeenEntry {
    char name[64];
    uint64_t at_ms; /* 事件首次到达的单调毫秒时刻（与 pc_events_now_ms 同基准） */
} g_ev_seen[16];
static int g_ev_seen_count = 0;

/* 进程内统一单调毫秒时钟（基准在首次调用时固定，仅相对比较有意义）。
 * Windows 使用 QPC 保证毫秒级分辨率（GetTickCount64 约 15.6ms 步进，
 * 无法满足 scenario 毫秒级门控）；POSIX 使用 CLOCK_MONOTONIC。 */
static uint64_t now_ms(void)
{
    static uint64_t base = 0;
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int freq_init = 0;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint64_t ticks = (uint64_t)counter.QuadPart;
    uint64_t f = (uint64_t)freq.QuadPart;
    uint64_t ms = (ticks / f) * 1000u + ((ticks % f) * 1000u) / f;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
    if (base == 0)
        base = ms;
    return ms - base;
}

static void lock_ev(void)
{
#ifdef _WIN32
    if (g_ev_lock_init) EnterCriticalSection(&g_ev_lock);
#else
    pthread_mutex_lock(&g_ev_lock);
#endif
}

static void unlock_ev(void)
{
#ifdef _WIN32
    if (g_ev_lock_init) LeaveCriticalSection(&g_ev_lock);
#else
    pthread_mutex_unlock(&g_ev_lock);
#endif
}

static void mark_seen(const char *event, uint64_t at_ms)
{
    lock_ev();
    for (int i = 0; i < g_ev_seen_count; i++)
        if (strcmp(g_ev_seen[i].name, event) == 0) {
            unlock_ev();
            return;
        }
    if (g_ev_seen_count < (int)(sizeof(g_ev_seen) / sizeof(g_ev_seen[0]))) {
        snprintf(g_ev_seen[g_ev_seen_count].name, sizeof(g_ev_seen[0].name),
                 "%s", event);
        g_ev_seen[g_ev_seen_count].at_ms = at_ms;
        g_ev_seen_count++;
    }
    unlock_ev();
}

int pc_events_open(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return DEMO_ERR_INVAL;
#ifdef _WIN32
    if (!g_ev_lock_init) {
        InitializeCriticalSection(&g_ev_lock);
        g_ev_lock_init = 1;
    }
#endif
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return DEMO_ERR;
    lock_ev();
    if (g_ev_file)
        fclose(g_ev_file);
    g_ev_file = file;
    g_ev_seq = 0;
    g_ev_seen_count = 0;
    unlock_ev();
    return DEMO_OK;
}

void pc_events_close(void)
{
    lock_ev();
    if (g_ev_file) {
        fflush(g_ev_file);
        fclose(g_ev_file);
        g_ev_file = NULL;
    }
    g_ev_observer = NULL;
    unlock_ev();
}

int pc_events_enabled(void)
{
    int enabled = 0;
    lock_ev();
    enabled = (g_ev_file != NULL);
    unlock_ev();
    return enabled;
}

void pc_events_emit(const pc_event_t *ev)
{
    if (ev == NULL || ev->event == NULL || ev->event[0] == '\0')
        return;

    uint64_t time = now_ms();
    pc_events_observer_fn observer;
    uint64_t seq = 0;
    int enabled = 0;
    lock_ev();
    observer = g_ev_observer;
    enabled = (g_ev_file != NULL);
    if (enabled)
        seq = ++g_ev_seq;
    unlock_ev();
    if (observer)
        observer(ev->event, time);
    mark_seen(ev->event, time);

    if (!enabled)
        return;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return;
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddNumberToObject(root, "seq", (double)seq);
    cJSON_AddNumberToObject(root, "time_ms", (double)time);
    cJSON_AddStringToObject(root, "role", "pc");
    cJSON_AddStringToObject(root, "event", ev->event);
    cJSON_AddStringToObject(root, "result", ev->result ? ev->result : "ok");
    cJSON_AddNumberToObject(root, "code", ev->code);
    cJSON_AddStringToObject(root, "device_id", ev->device_id ? ev->device_id : "");
    if (ev->data_json && ev->data_json[0]) {
        cJSON *data = cJSON_Parse(ev->data_json);
        cJSON_AddItemToObject(root, "data", data ? data : cJSON_CreateObject());
    } else {
        cJSON_AddObjectToObject(root, "data");
    }

    char *line = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (line == NULL)
        return;

    lock_ev();
    if (g_ev_file) {
        fputs(line, g_ev_file);
        fputc('\n', g_ev_file);
        fflush(g_ev_file);
    }
    unlock_ev();
    free(line);
}

void pc_events_set_observer(pc_events_observer_fn observer)
{
    lock_ev();
    g_ev_observer = observer;
    unlock_ev();
}

int pc_events_was_seen(const char *event)
{
    if (event == NULL)
        return 0;
    lock_ev();
    for (int i = 0; i < g_ev_seen_count; i++)
        if (strcmp(g_ev_seen[i].name, event) == 0) {
            unlock_ev();
            return 1;
        }
    unlock_ev();
    return 0;
}

uint64_t pc_events_now_ms(void)
{
    return now_ms();
}

uint64_t pc_events_last_seen_ms(const char *event)
{
    if (event == NULL)
        return 0;
    lock_ev();
    for (int i = 0; i < g_ev_seen_count; i++)
        if (strcmp(g_ev_seen[i].name, event) == 0) {
            uint64_t at = g_ev_seen[i].at_ms;
            unlock_ev();
            return at;
        }
    unlock_ev();
    return 0;
}
