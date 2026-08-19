#ifndef DEMO_DEVICE_LIMITS_H
#define DEMO_DEVICE_LIMITS_H

#include "device_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 滑动窗口限速：1s 窗口内发送计数；超限且非心跳 → 返回 0（丢弃） */
int limits_allow_send(device_app_t *app, int is_heartbeat);

#ifdef __cplusplus
}
#endif

#endif
