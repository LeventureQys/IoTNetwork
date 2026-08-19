cmake_minimum_required(VERSION 3.20)
project(device_ss03_core_tests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif()

enable_testing()

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(DEVICE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
set(DEVICE_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
set(DEVICE_CJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/cjson")

if(WIN32)
    set(DEVICE_PLATFORM_SRC "${DEVICE_SRC_DIR}/platform/win32/device_platform_win32.c")
else()
    set(DEVICE_PLATFORM_SRC "${DEVICE_SRC_DIR}/platform/posix/device_platform_posix.c")
endif()

add_library(device_ss03_core_prod STATIC
    ${DEVICE_SRC_DIR}/common/frame.c
    ${DEVICE_SRC_DIR}/common/log.c
    ${DEVICE_SRC_DIR}/common/net_abstraction.c
    ${DEVICE_SRC_DIR}/common/protocol.c
    ${DEVICE_SRC_DIR}/config/device_config.c
    ${DEVICE_SRC_DIR}/config/device_path.c
    ${DEVICE_SRC_DIR}/runtime/backend_factory.c
    ${DEVICE_SRC_DIR}/core/device_app.c
    ${DEVICE_SRC_DIR}/core/device_eventlog.c
    ${DEVICE_SRC_DIR}/core/device_limits.c
    ${DEVICE_SRC_DIR}/core/device_session.c
    ${DEVICE_SRC_DIR}/core/device_heal.c
    ${DEVICE_PLATFORM_SRC}
    ${DEVICE_CJSON_DIR}/cJSON.c
)
target_include_directories(device_ss03_core_prod PUBLIC
    ${DEVICE_INC_DIR}
    ${DEVICE_SRC_DIR}/core
    ${DEVICE_SRC_DIR}/config
    ${DEVICE_SRC_DIR}/platform
    ${DEVICE_CJSON_DIR}
)

if(WIN32)
    target_link_libraries(device_ss03_core_prod PUBLIC ws2_32)
endif()

# googletest（复制自 demo/third_party/googletest-1.15.2）；SS06 聚合时由
# tests/CMakeLists.txt 统一添加一次，此处仅在独立构建（未聚合）时添加。
if(NOT TARGET gtest_main)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/googletest-1.15.2"
                     "${CMAKE_CURRENT_BINARY_DIR}/googletest")
endif()
include(GoogleTest)

add_executable(device_core_tests
    test_core_eventlog.cpp
    test_core_limits_heal.cpp
    test_core_sm.cpp
    fake_backend.c
)
target_link_libraries(device_core_tests PRIVATE device_ss03_core_prod gtest_main)
gtest_discover_tests(device_core_tests)
