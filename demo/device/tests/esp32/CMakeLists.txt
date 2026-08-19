cmake_minimum_required(VERSION 3.16)
project(demo_device_tests_esp32 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 头文件定位：固定 device/include（SS06 聚合时可预置 DEMO_DEVICE_INCLUDE_DIR
# 覆盖；不再支持根级回退）。
# ---------------------------------------------------------------------------
if(NOT DEFINED DEMO_DEVICE_INCLUDE_DIR)
    set(DEMO_DEVICE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
endif()

set(ESP32C2_BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../backends/esp32c2")

# 宿主检查：编译 ESP32-C2 骨架 + 运行 vtable/占位行为断言。
# 注意：这是纯 C 语法/接口完整性检查，不是 ESP-IDF build，不得报告为
# ESP-IDF 构建通过。
add_executable(esp32_host_check
    esp32_host_check.c
    "${ESP32C2_BACKEND_DIR}/esp32c2_backend.c"
)
target_include_directories(esp32_host_check PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ESP32C2_BACKEND_DIR}/include"
    "${DEMO_DEVICE_INCLUDE_DIR}"
)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME esp32_host_check COMMAND esp32_host_check)
endif()
