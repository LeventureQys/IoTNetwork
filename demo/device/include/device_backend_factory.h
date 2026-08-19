#ifndef DEMO_DEVICE_DEVICE_BACKEND_FACTORY_H
#define DEMO_DEVICE_DEVICE_BACKEND_FACTORY_H

#include <stdint.h>
#include "net_abstraction.h"
#include "device_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct device_backend_instance {
    const net_backend_t *vtable;
    void *user;
    void (*destroy_user)(void *user);
} device_backend_instance_t;

typedef struct device_sim_backend_options {
    const char *config_path;
    const char *nvs_file;
    const char *sim_catalog_dir;
    const char *target_ssid;
    const char *target_password;
    const char *host_virtual_ip;
    unsigned int device_index;
    unsigned int provision_port;
    uint32_t random_seed;
} device_sim_backend_options_t;

typedef struct device_linux_backend_options {
    const char *config_path;
    const char *nvs_file;
    const char *hotspot_config_path;
    const char *sta_interface;
    unsigned int device_index;
} device_linux_backend_options_t;

device_result_t device_sim_backend_create(
    const device_sim_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error);

device_result_t device_linux_backend_create(
    const device_linux_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error);

device_result_t device_backend_create_for_host(
    device_backend_kind_t kind,
    const device_sim_backend_options_t *sim_options,
    const device_linux_backend_options_t *linux_options,
    device_backend_instance_t *out_instance,
    device_error_t *error);

void device_backend_instance_destroy(device_backend_instance_t *instance);

#ifdef __cplusplus
}
#endif

#endif
