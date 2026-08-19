/*
 * ESP32-C2 设备入口（纯 C）。
 *
 * 组合方式（设计文档 §8.6 / §8.2.1）：
 * - 不使用宿主 dispatcher（device_backend_create_for_host 对 ESP32C2
 *   恒返回 DEVICE_ERR_NOT_SUPPORTED）；
 * - 直接组合 ESP32-C2 后端 vtable（esp32c2_backend_get）与设备平台无关
 *   runtime（net_ctx_create + src/core 的 device_app）；
 * - 本文件只依赖 device 目录内部头文件与纯 C 模块。
 *
 * 依赖（SS03 已落地）：
 * - device/include：net_abstraction.h / device_config.h / log.h
 * - device/src/core：device_app.h
 * 在 ESP-IDF 环境下 app_main 是真正入口；本工程 idf.py build 需 ESP-IDF
 * 工具链（本仓库开发机为 Windows，见 README 阻塞说明）。
 */
#include <string.h>

#include "esp32c2_impl.h"
#include "net_abstraction.h"
#include "log.h"

#if defined(__has_include)
#  if __has_include("device_app.h")
#    include "device_app.h"
#    define SS05_DEVICE_APP_AVAILABLE 1
#  endif
#  if __has_include("device_config.h")
#    include "device_config.h"
#    define SS05_DEVICE_CONFIG_AVAILABLE 1
#  endif
#endif

#define FIRMWARE_DEVICE_ID "AA:BB:CC:DD:EE:FF"

void app_main(void)
{
    net_ctx_t *net = NULL;
    int result;

    log_init(LOG_INFO);

    /* 骨架阶段：后端 vtable 可获取（esp32c2_backend_get），但所有能力
     * 接口均为占位（返回 DEMO_ERR）。在真实实现落地前不宣称可用。 */
    result = net_ctx_create(esp32c2_backend_get(), NULL, NULL, &net);
    if (result != DEMO_OK || net == NULL) {
        /* 骨架阶段 net_ctx_create 不应失败；失败即挂起（不弹窗、不退出） */
        for (;;) {
            /* no-op */
        }
    }

#if defined(SS05_DEVICE_APP_AVAILABLE) && defined(SS05_DEVICE_CONFIG_AVAILABLE)
    {
        device_config_t params;
        char config_dir[256] = {0};
        int missing = 0;
        device_app_t *app = NULL;

        device_config_defaults(&params);
        device_config_load(&params, NULL, config_dir, sizeof(config_dir),
                           &missing);
        app = device_app_create(&params, net, FIRMWARE_DEVICE_ID, 0x5eed);
        if (app != NULL) {
            device_app_run(app); /* 阻塞事件循环 */
            device_app_destroy(app);
        }
    }
#endif

    net_ctx_destroy(net);
}
