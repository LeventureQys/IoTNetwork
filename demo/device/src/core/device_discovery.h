#ifndef DEMO_DEVICE_DISCOVERY_H
#define DEMO_DEVICE_DISCOVERY_H

#include "device_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

int  discovery_start(device_app_t *app);   /* 重置轮次与快速窗口 */
int  discovery_poll(device_app_t *app);    /* 返回 1=已获得 host 地址（写 sess_host） */
void discovery_stop(device_app_t *app);

#ifdef __cplusplus
}
#endif

#endif
