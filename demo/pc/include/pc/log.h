#ifndef DEMO_LOG_H
#define DEMO_LOG_H
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

void log_init(int level);
void log_set_level(int level);
int log_set_file(const char *path);
void log_close_file(void);
void log_msg(int level, const char *module, const char *fmt, ...);

/* 日志订阅（UI 用）：注册后每条日志在锁外回调（线程安全，单订阅者） */
typedef void (*log_sink_fn)(int level, const char *module, const char *msg);
void log_set_sink(log_sink_fn sink);

#define LOG_T(module, ...) log_msg(LOG_TRACE, module, __VA_ARGS__)
#define LOG_D(module, ...) log_msg(LOG_DEBUG, module, __VA_ARGS__)
#define LOG_I(module, ...) log_msg(LOG_INFO, module, __VA_ARGS__)
#define LOG_W(module, ...) log_msg(LOG_WARN, module, __VA_ARGS__)
#define LOG_E(module, ...) log_msg(LOG_ERROR, module, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
