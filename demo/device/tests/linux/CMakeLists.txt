cmake_minimum_required(VERSION 3.16)
project(demo_device_tests_linux LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 依赖定位（SS06 聚合时可在父级预置变量覆盖；不再支持根级回退）：
#   DEMO_DEVICE_INCLUDE_DIR  device/include
#   DEMO_DEVICE_CJSON_DIR    device/third_party/cjson
#   DEMO_DEVICE_COMMON_DIR   device/src/common
#   DEMO_DEVICE_GTEST_DIR    device/third_party/googletest-1.15.2
# ---------------------------------------------------------------------------
if(NOT DEFINED DEMO_DEVICE_INCLUDE_DIR)
    set(DEMO_DEVICE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
endif()
if(NOT DEFINED DEMO_DEVICE_CJSON_DIR)
    set(DEMO_DEVICE_CJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cjson")
endif()
if(NOT DEFINED DEMO_DEVICE_COMMON_DIR)
    set(DEMO_DEVICE_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/common")
endif()
if(NOT DEFINED DEMO_DEVICE_GTEST_DIR)
    set(DEMO_DEVICE_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/googletest-1.15.2")
endif()

set(LINUX_BACKEND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../backends/linux")

# ---------------------------------------------------------------------------
# 跨平台检查：非 Linux 平台的 device_linux_backend_create stub。
# 不依赖 gtest / POSIX，任何平台（含 Windows）均可编译运行。
# ---------------------------------------------------------------------------
add_executable(device_linux_unavailable_check
    device_linux_unavailable_check.c
    "${LINUX_BACKEND_DIR}/device_linux_unavailable.c"
)
target_include_directories(device_linux_unavailable_check PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LINUX_BACKEND_DIR}"
    "${DEMO_DEVICE_INCLUDE_DIR}"
)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    include(CTest)
endif()
if(BUILD_TESTING)
    add_test(NAME device_linux_unavailable_check
             COMMAND device_linux_unavailable_check)
endif()

# ---------------------------------------------------------------------------
# Linux 平台测试（真实热点/STA/NVS 代码；fake ops 在普通 Linux 上自动运行，
# 不需要 root、hostapd、dnsmasq 或 AP 网卡）。
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    if(NOT TARGET gtest_main)
        add_subdirectory("${DEMO_DEVICE_GTEST_DIR}"
                         "${CMAKE_CURRENT_BINARY_DIR}/googletest")
    endif()

    set(LINUX_COMMON_SOURCES
        "${DEMO_DEVICE_COMMON_DIR}/log.c"
        "${DEMO_DEVICE_CJSON_DIR}/cJSON.c"
    )
    set(LINUX_COMMON_INCLUDE_DIRS
        "${LINUX_BACKEND_DIR}"
        "${DEMO_DEVICE_INCLUDE_DIR}"
        "${DEMO_DEVICE_CJSON_DIR}"
    )
    set(LINUX_HOTSPOT_SOURCES
        "${LINUX_BACKEND_DIR}/linux_hotspot.c"
        "${LINUX_BACKEND_DIR}/linux_hotspot_config.c"
        "${LINUX_BACKEND_DIR}/linux_hotspot_linux_ops.c"
        "${LINUX_BACKEND_DIR}/linux_exec.c"
    )
    set(LINUX_WIFI_SOURCES
        "${LINUX_BACKEND_DIR}/linux_wifi.c"
    )
    set(LINUX_NVS_SOURCES
        "${LINUX_BACKEND_DIR}/linux_nvs.c"
    )
    set(LINUX_SOCKET_SOURCES
        "${LINUX_BACKEND_DIR}/linux_socket.c"
    )
    set(LINUX_BACKEND_SOURCES
        "${LINUX_BACKEND_DIR}/device_linux_backend.c"
    )

    add_executable(test_linux_hotspot_config test_linux_hotspot_config.cpp
        "${LINUX_BACKEND_DIR}/linux_hotspot_config.c"
        "${DEMO_DEVICE_CJSON_DIR}/cJSON.c"
    )
    target_include_directories(test_linux_hotspot_config PRIVATE
        ${LINUX_COMMON_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(test_linux_hotspot_config PRIVATE gtest_main)
    target_compile_options(test_linux_hotspot_config PRIVATE -pthread)

    add_executable(test_linux_hotspot_runtime test_linux_hotspot_runtime.cpp
        ${LINUX_HOTSPOT_SOURCES}
        ${LINUX_COMMON_SOURCES}
    )
    target_include_directories(test_linux_hotspot_runtime PRIVATE
        ${LINUX_COMMON_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(test_linux_hotspot_runtime PRIVATE gtest_main)
    target_compile_options(test_linux_hotspot_runtime PRIVATE -pthread)

    add_executable(test_linux_backend test_linux_backend.cpp
        ${LINUX_BACKEND_SOURCES}
        ${LINUX_HOTSPOT_SOURCES}
        ${LINUX_WIFI_SOURCES}
        ${LINUX_NVS_SOURCES}
        ${LINUX_SOCKET_SOURCES}
        ${LINUX_COMMON_SOURCES}
    )
    target_include_directories(test_linux_backend PRIVATE
        ${LINUX_COMMON_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(test_linux_backend PRIVATE gtest_main)
    target_compile_options(test_linux_backend PRIVATE -pthread)
    # linux_socket.c 使用 accept4/MSG_NOSIGNAL，需要 _GNU_SOURCE
    target_compile_definitions(test_linux_backend PRIVATE _GNU_SOURCE)

    if(BUILD_TESTING)
        add_test(NAME test_linux_hotspot_config COMMAND test_linux_hotspot_config)
        add_test(NAME test_linux_hotspot_runtime COMMAND test_linux_hotspot_runtime)
        add_test(NAME test_linux_backend COMMAND test_linux_backend)
    endif()

endif()
