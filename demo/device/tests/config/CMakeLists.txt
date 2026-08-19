cmake_minimum_required(VERSION 3.20)
project(device_ss03_config_tests LANGUAGES C CXX)

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

add_library(device_ss03_config_prod STATIC
    ${DEVICE_SRC_DIR}/common/log.c
    ${DEVICE_SRC_DIR}/config/device_config.c
    ${DEVICE_SRC_DIR}/config/device_path.c
    ${DEVICE_SRC_DIR}/config/host_options.c
    ${DEVICE_CJSON_DIR}/cJSON.c
)
target_include_directories(device_ss03_config_prod PUBLIC
    ${DEVICE_INC_DIR}
    ${DEVICE_SRC_DIR}/config
    ${DEVICE_CJSON_DIR}
)

# googletest（复制自 demo/third_party/googletest-1.15.2）；SS06 聚合时由
# tests/CMakeLists.txt 统一添加一次，此处仅在独立构建（未聚合）时添加。
if(NOT TARGET gtest_main)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/googletest-1.15.2"
                     "${CMAKE_CURRENT_BINARY_DIR}/googletest")
endif()
include(GoogleTest)

add_executable(device_config_tests
    test_device_config.cpp
    test_contract.cpp
    test_host_options.cpp
)
target_compile_definitions(device_config_tests PRIVATE
    DEVICE_CONTRACT_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/../../../integration_tests/contracts/protocol-contract.golden.json")
target_link_libraries(device_config_tests PRIVATE device_ss03_config_prod gtest_main)
gtest_discover_tests(device_config_tests)
