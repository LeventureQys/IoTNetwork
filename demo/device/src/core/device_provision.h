#ifndef DEMO_DEVICE_PROVISION_H
#define DEMO_DEVICE_PROVISION_H

#include "device_priv.h"
#ifdef __cplusplus
extern "C" {
#endif

#define PROV_RET_OK         0   /* 无活动 */
#define PROV_RET_ACTIVE     1   /* 有活动 */
#define PROV_RET_AP_LOCKED  2   /* PIN 锁定，需退避 */

int  prov_server_start(device_app_t *app);
int  prov_server_poll(device_app_t *app);
void prov_server_stop(device_app_t *app);
void prov_server_on_msg(device_app_t *app, cJSON *json);
void prov_server_on_conn_closed(device_app_t *app);

#ifdef __cplusplus
}
#endif

#endif
