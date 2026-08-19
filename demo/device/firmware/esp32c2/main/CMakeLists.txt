# main 组件：纯 C 入口 + ESP32-C2 后端 + 设备平台无关 runtime/核心。
#
# 只引用 device 目录内部（../../include、../../src/common、../../src/core、
# ../../backends/esp32c2、../../third_party/cjson）；不得引用仓库根目录或任何 C++/Qt。
#
# 说明：src/core/device_app.c/.h 依赖 cJSON（device/third_party/cjson，SS03
# 复制），因此本组件一并纳入 cJSON.c 与头目录（仍在 device 目录内部）。
idf_component_register(
    SRCS
        "app_main.c"
        "../../backends/esp32c2/esp32c2_backend.c"
        "../../src/common/net_abstraction.c"
        "../../src/common/log.c"
        "../../src/common/protocol.c"
        "../../src/common/frame.c"
        "../../third_party/cjson/cJSON.c"
    INCLUDE_DIRS
        "."
        "../../include"
        "../../src/common"
        "../../src/core"
        "../../backends/esp32c2/include"
        "../../third_party/cjson"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        lwip
        mdns
)
