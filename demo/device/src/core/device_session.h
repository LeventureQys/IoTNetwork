#ifndef DEMO_DEVICE_SESSION_H
#define DEMO_DEVICE_SESSION_H

#include "device_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

int  session_connect(device_app_t *app, const net_addr_t *host);
int  session_poll(device_app_t *app);       /* 返回 1=存活 0=失效 */
void session_disconnect(device_app_t *app);
void session_on_msg(device_app_t *app, cJSON *json);
void session_on_conn_closed(device_app_t *app);

#ifdef __cplusplus
}
#endif

#endif
