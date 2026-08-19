#include "device_linux_backend.h"

#include <stdio.h>
#include <string.h>

/*
 * 非 Linux 平台的 device_linux_backend_create 独立 stub。
 *
 * 契约（设计文档 §8.2.1 / §8.6）：
 * - 仅此一个符号，不含任何真实热点/STA/NVS/socket 源码；
 * - 清零输出并返回 DEVICE_ERR_NOT_SUPPORTED，绝不回退 sim，绝不返回成功。
 */

device_result_t device_linux_backend_create(
    const device_linux_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error)
{
    (void)options;
    if (out_instance != NULL)
        memset(out_instance, 0, sizeof(*out_instance));
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->code = DEVICE_ERR_NOT_SUPPORTED;
        snprintf(error->operation, sizeof(error->operation), "%s",
                 "device_linux_backend_create");
        snprintf(error->message, sizeof(error->message), "%s",
                 "真实 Linux 热点/STA 后端仅支持 Linux 平台");
    }
    return DEVICE_ERR_NOT_SUPPORTED;
}
