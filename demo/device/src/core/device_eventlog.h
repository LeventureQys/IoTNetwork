#ifndef DEMO_DEVICE_EVENTLOG_H
#define DEMO_DEVICE_EVENTLOG_H

#include "device_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

void evlog_record(device_app_t *app, const char *fmt, ...);
int  evlog_fill_report(device_app_t *app, char *out, int cap);
void evlog_flush(device_app_t *app);   /* 强制持久化（销毁前调用） */

#ifdef __cplusplus
}
#endif

#endif
