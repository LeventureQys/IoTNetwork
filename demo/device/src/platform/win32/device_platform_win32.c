#include "device_platform.h"
#include <windows.h>
#include <stdlib.h>

struct device_thread {
    HANDLE handle;
    HANDLE done_event;
    void (*entry)(void *user);
    void *arg;
};

struct device_mutex {
    CRITICAL_SECTION cs;
};

struct device_cond {
    CONDITION_VARIABLE cv;
};

static DWORD WINAPI thread_entry_win(LPVOID user)
{
    device_thread_t *t = (device_thread_t *)user;
    t->entry(t->arg);
    SetEvent(t->done_event);
    return 0;
}

static device_thread_t *plat_thread_create(void (*entry)(void *user), void *user)
{
    device_thread_t *t = (device_thread_t *)calloc(1, sizeof(device_thread_t));
    if (t == NULL)
        return NULL;
    t->done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (t->done_event == NULL) {
        free(t);
        return NULL;
    }
    t->entry = entry;
    t->arg = user;
    t->handle = CreateThread(NULL, 0, thread_entry_win, t, 0, NULL);
    if (t->handle == NULL) {
        CloseHandle(t->done_event);
        free(t);
        return NULL;
    }
    return t;
}

static int plat_thread_join(device_thread_t *t, uint32_t timeout_ms)
{
    if (t == NULL)
        return DEVICE_PLAT_OK;
    DWORD ms = (timeout_ms == UINT32_MAX) ? INFINITE : (DWORD)timeout_ms;
    DWORD rc = WaitForSingleObject(t->done_event, ms);
    return (rc == WAIT_OBJECT_0) ? DEVICE_PLAT_OK : DEVICE_PLAT_TIMEOUT;
}

static void plat_thread_destroy(device_thread_t *t)
{
    if (t == NULL)
        return;
    if (t->handle != NULL) {
        WaitForSingleObject(t->handle, INFINITE);
        CloseHandle(t->handle);
    }
    if (t->done_event != NULL)
        CloseHandle(t->done_event);
    free(t);
}

static device_mutex_t *plat_mutex_create(void)
{
    device_mutex_t *m = (device_mutex_t *)malloc(sizeof(device_mutex_t));
    if (m == NULL)
        return NULL;
    InitializeCriticalSection(&m->cs);
    return m;
}

static void plat_mutex_destroy(device_mutex_t *m)
{
    if (m == NULL)
        return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

static void plat_mutex_lock(device_mutex_t *m)
{
    if (m == NULL)
        return;
    EnterCriticalSection(&m->cs);
}

static void plat_mutex_unlock(device_mutex_t *m)
{
    if (m == NULL)
        return;
    LeaveCriticalSection(&m->cs);
}

static device_cond_t *plat_cond_create(void)
{
    device_cond_t *c = (device_cond_t *)malloc(sizeof(device_cond_t));
    if (c == NULL)
        return NULL;
    InitializeConditionVariable(&c->cv);
    return c;
}

static void plat_cond_destroy(device_cond_t *c)
{
    if (c == NULL)
        return;
    free(c);
}

static void plat_cond_wait(device_cond_t *c, device_mutex_t *m)
{
    if (c == NULL || m == NULL)
        return;
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
}

static int plat_cond_wait_timeout(device_cond_t *c, device_mutex_t *m, uint32_t timeout_ms)
{
    if (c == NULL || m == NULL)
        return DEVICE_PLAT_TIMEOUT;
    DWORD ms = (timeout_ms == UINT32_MAX) ? INFINITE : (DWORD)timeout_ms;
    if (!SleepConditionVariableCS(&c->cv, &m->cs, ms))
        return DEVICE_PLAT_TIMEOUT;
    return DEVICE_PLAT_OK;
}

static void plat_cond_signal(device_cond_t *c)
{
    if (c == NULL)
        return;
    WakeConditionVariable(&c->cv);
}

static void plat_cond_broadcast(device_cond_t *c)
{
    if (c == NULL)
        return;
    WakeAllConditionVariable(&c->cv);
}

static uint64_t plat_monotonic_ms(void)
{
    static LARGE_INTEGER freq;
    static int freq_ready = 0;
    if (!freq_ready) {
        QueryPerformanceFrequency(&freq);
        freq_ready = 1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((now.QuadPart * 1000LL) / freq.QuadPart);
}

const device_platform_t device_platform_default = {
    plat_thread_create,
    plat_thread_join,
    plat_thread_destroy,
    plat_mutex_create,
    plat_mutex_destroy,
    plat_mutex_lock,
    plat_mutex_unlock,
    plat_cond_create,
    plat_cond_destroy,
    plat_cond_wait,
    plat_cond_wait_timeout,
    plat_cond_signal,
    plat_cond_broadcast,
    plat_monotonic_ms
};
