#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_lock;
static int g_log_lock_init = 0;
#else
#include <pthread.h>
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static int g_log_level = LOG_INFO;
static volatile log_sink_fn g_log_sink = NULL;
static FILE *g_log_file = NULL;

void log_set_sink(log_sink_fn sink)
{
    g_log_sink = sink;
}

void log_init(int level)
{
    g_log_level = level;
#ifdef _WIN32
    InitializeCriticalSection(&g_log_lock);
    g_log_lock_init = 1;
#endif
}

void log_set_level(int level)
{
    g_log_level = level;
}

int log_set_file(const char *path)
{
    FILE *file;
    if (path == NULL || path[0] == '\0')
        return DEMO_ERR_INVAL;
    file = fopen(path, "ab");
    if (file == NULL)
        return DEMO_ERR;

#ifdef _WIN32
    if (g_log_lock_init) EnterCriticalSection(&g_log_lock);
#else
    pthread_mutex_lock(&g_log_lock);
#endif
    if (g_log_file != NULL)
        fclose(g_log_file);
    g_log_file = file;
#ifdef _WIN32
    if (g_log_lock_init) LeaveCriticalSection(&g_log_lock);
#else
    pthread_mutex_unlock(&g_log_lock);
#endif
    return DEMO_OK;
}

void log_close_file(void)
{
#ifdef _WIN32
    if (g_log_lock_init) EnterCriticalSection(&g_log_lock);
#else
    pthread_mutex_lock(&g_log_lock);
#endif
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
#ifdef _WIN32
    if (g_log_lock_init) LeaveCriticalSection(&g_log_lock);
#else
    pthread_mutex_unlock(&g_log_lock);
#endif
}

static const char *level_str(int level)
{
    switch (level) {
    case LOG_TRACE: return "跟踪";
    case LOG_DEBUG: return "调试";
    case LOG_INFO:  return "信息";
    case LOG_WARN:  return "警告";
    case LOG_ERROR: return "错误";
    default:        return "?????";
    }
}

void log_msg(int level, const char *module, const char *fmt, ...)
{
    if (level < g_log_level || fmt == NULL || fmt[0] == '\0')
        return;

    char timebuf[64];
#ifdef _WIN32
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    }
#else
    {
        struct timespec ts;
        struct tm tm;
        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tm);
        snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    }
#endif

    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

#ifdef _WIN32
    if (g_log_lock_init) EnterCriticalSection(&g_log_lock);
#else
    pthread_mutex_lock(&g_log_lock);
#endif

    if (level >= LOG_ERROR)
        fprintf(stderr, "[%s][%s][%s] %s\n", timebuf, level_str(level), module ? module : "-", msg);
    else
        printf("[%s][%s][%s] %s\n", timebuf, level_str(level), module ? module : "-", msg);
    if (g_log_file != NULL) {
        fprintf(g_log_file, "[%s][%s][%s] %s\n", timebuf, level_str(level),
                module ? module : "-", msg);
        fflush(g_log_file);
    }
    fflush(stdout);
    fflush(stderr);

#ifdef _WIN32
    if (g_log_lock_init) LeaveCriticalSection(&g_log_lock);
#else
    pthread_mutex_unlock(&g_log_lock);
#endif

    /* 锁外回调订阅者（UI 日志推送），避免与 log 锁形成嵌套 */
    log_sink_fn sink = g_log_sink;
    if (sink)
        sink(level, module ? module : "-", msg);
}
