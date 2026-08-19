cmake_minimum_required(VERSION 3.16)
project(demo_device_backend_linux LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 头文件定位（SS06 聚合时可在父级预置以下变量覆盖；不再支持根级回退）：
#   DEMO_DEVICE_INCLUDE_DIR   device/include
#   DEMO_DEVICE_CJSON_DIR     device/third_party/cjson
# ---------------------------------------------------------------------------
if(NOT DEFINED DEMO_DEVICE_INCLUDE_DIR)
    set(DEMO_DEVICE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
endif()
if(NOT DEFINED DEMO_DEVICE_CJSON_DIR)
    set(DEMO_DEVICE_CJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cjson")
endif()

set(LINUX_BACKEND_SOURCES
    device_linux_backend.c
    linux_exec.c
    linux_hotspot.c
    linux_hotspot_config.c
    linux_hotspot_linux_ops.c
    linux_nvs.c
    linux_socket.c
    linux_wifi.c
)

# Linux 平台：真实后端 device_backend_linux。
# 非 Linux 平台：仅编译 device_backend_linux_unavailable（NOT_SUPPORTED stub），
# 不含任何热点源码。两个目标都提供符号 device_linux_backend_create。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(device_backend_linux STATIC ${LINUX_BACKEND_SOURCES})
    target_include_directories(device_backend_linux PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${DEMO_DEVICE_INCLUDE_DIR}"
        "${DEMO_DEVICE_CJSON_DIR}"
    )
    target_compile_options(device_backend_linux PRIVATE -Wall -Wextra)
    target_compile_definitions(device_backend_linux PRIVATE _GNU_SOURCE)
else()
    add_library(device_backend_linux_unavailable STATIC device_linux_unavailable.c)
    target_include_directories(device_backend_linux_unavailable PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${DEMO_DEVICE_INCLUDE_DIR}"
    )
endif()
