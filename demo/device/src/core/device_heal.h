#ifndef DEMO_DEVICE_HEAL_H
#define DEMO_DEVICE_HEAL_H

#include "device_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

void heal_on_disconnect(device_app_t *app, const char *reason);
int  heal_next_backoff_ms(device_app_t *app);
void heal_reset_backoff(device_app_t *app);
int  heal_rssi_poll(device_app_t *app);   /* 返回 1=需主动重连 */

#ifdef __cplusplus
}
#endif

#endif
