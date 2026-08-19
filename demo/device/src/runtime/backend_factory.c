#include "device_backend_factory.h"
#include <string.h>

device_result_t device_backend_create_for_host(
    device_backend_kind_t kind,
    const device_sim_backend_options_t *sim_options,
    const device_linux_backend_options_t *linux_options,
    device_backend_instance_t *out_instance,
    device_error_t *error)
{
    if (out_instance != NULL)
        memset(out_instance, 0, sizeof(*out_instance));
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (out_instance == NULL || error == NULL)
        return DEVICE_ERR_INVALID_ARGUMENT;

    switch (kind) {
    case DEVICE_BACKEND_SIM:
        return device_sim_backend_create(sim_options, out_instance, error);
    case DEVICE_BACKEND_LINUX:
#ifdef __linux__
        return device_linux_backend_create(linux_options, out_instance, error);
#else
        return DEVICE_ERR_NOT_SUPPORTED;
#endif
    case DEVICE_BACKEND_ESP32C2:
        /* 宿主 Qt 程序选择 ESP32C2 始终 NOT_SUPPORTED；ESP-IDF 直接组合 ESP32 后端 */
        return DEVICE_ERR_NOT_SUPPORTED;
    default:
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
}

void device_backend_instance_destroy(device_backend_instance_t *instance)
{
    if (instance == NULL)
        return;
    if (instance->destroy_user != NULL && instance->user != NULL)
        instance->destroy_user(instance->user);
    memset(instance, 0, sizeof(*instance));
}
