/*
 * 非 Linux 平台 device_linux_backend_create stub 的宿主检查（跨平台）。
 *
 * 验证契约（设计文档 §8.2.1）：
 * - 输出实例被清零；
 * - 返回 DEVICE_ERR_NOT_SUPPORTED，绝不返回成功；
 * - 不包含任何真实热点/STA 源码（链接上不存在）。
 * 可在任意平台（含 Windows）编译运行。
 */
#include "device_linux_backend.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    device_backend_instance_t instance;
    device_error_t error;
    device_linux_backend_options_t options;
    device_result_t result;
    int failures = 0;

    memset(&instance, 0xAA, sizeof(instance));
    memset(&error, 0xAA, sizeof(error));
    memset(&options, 0, sizeof(options));
    options.device_index = 0;
    options.config_path = "config/device_linux.json";

    result = device_linux_backend_create(&options, &instance, &error);
    if (result != DEVICE_ERR_NOT_SUPPORTED) {
        fprintf(stderr, "FAIL: 期望 DEVICE_ERR_NOT_SUPPORTED，得到 %d\n",
                (int)result);
        ++failures;
    }
    if (instance.vtable != NULL || instance.user != NULL ||
        instance.destroy_user != NULL) {
        fprintf(stderr, "FAIL: 输出实例未清零\n");
        ++failures;
    }
    if (error.code != DEVICE_ERR_NOT_SUPPORTED) {
        fprintf(stderr, "FAIL: error.code 未设置\n");
        ++failures;
    }

    /* 带 NULL options 也必须遵守相同契约 */
    result = device_linux_backend_create(NULL, &instance, &error);
    if (result != DEVICE_ERR_NOT_SUPPORTED || instance.vtable != NULL ||
        instance.user != NULL || instance.destroy_user != NULL) {
        fprintf(stderr, "FAIL: NULL options 契约被违反\n");
        ++failures;
    }

    if (failures == 0) {
        printf("device_linux_unavailable_check: PASS\n");
        return 0;
    }
    fprintf(stderr, "device_linux_unavailable_check: FAILED with %d checks\n",
            failures);
    return 1;
}
