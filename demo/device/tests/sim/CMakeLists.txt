cmake_minimum_required(VERSION 3.20)
project(DeviceSimBackendTests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# 相对路径引用 backends/sim 生产源码与 device 私有 cjson/真实契约头
# （SS06 聚合：sim/include 临时契约头已删除，include 切换到 device/include；
#  cJSON 用 device/third_party 私有副本，不再引用根级目录）。
set(SIM_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../backends/sim")
set(DEVICE_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
set(DEVICE_TP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")

# googletest（复制自 demo/third_party/googletest-1.15.2）；SS06 聚合时由
# tests/CMakeLists.txt 统一添加一次，此处仅在独立构建（未聚合）时添加。
if(NOT TARGET gtest_main)
    add_subdirectory("${DEVICE_TP_DIR}/googletest-1.15.2" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
endif()

# 生产代码全部以 C 编译器编译（本项目 LANGUAGES C CXX，.c 自动走 C11）。
add_library(device_sim_backend_c STATIC
    "${SIM_SRC_DIR}/sim_backend.c"
    "${SIM_SRC_DIR}/sim_world.c"
    "${SIM_SRC_DIR}/sim_socket.c"
    "${SIM_SRC_DIR}/sim_tcp_endpoint.c"
    "${SIM_SRC_DIR}/sim_nvs.c"
    "${SIM_SRC_DIR}/sim_ap_catalog.c"
    "${SIM_SRC_DIR}/sim_mdns.c"
    "${SIM_SRC_DIR}/sim_fault.c"
    "${SIM_SRC_DIR}/sim_random.c"
    "${DEVICE_TP_DIR}/cjson/cJSON.c"
)
target_include_directories(device_sim_backend_c PUBLIC
    "${DEVICE_INC_DIR}"
    "${SIM_SRC_DIR}"
    "${DEVICE_TP_DIR}/cjson"
)
if(MSVC)
    # 源文件为 UTF-8（无 BOM）注释/字符串；与旧 DemoShared.cmake 的 /utf-8 一致
    target_compile_options(device_sim_backend_c PRIVATE /utf-8)
endif()
if(WIN32)
    target_link_libraries(device_sim_backend_c PUBLIC ws2_32)
endif()

# SS03 的 device/src/common（net_abstraction.c 等）尚未迁移；SS06 聚合时
# 在此追加：
# if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/common") ... endif()

add_executable(device_sim_tests
    test_sim_world.cpp
    test_sim_tcp_endpoint.cpp
    test_sim_nvs.cpp
    test_sim_socket.cpp
    test_sim_ap_catalog.cpp
    test_sim_backend.cpp
    test_sim_random.cpp
)
target_include_directories(device_sim_tests PRIVATE
    "${DEVICE_INC_DIR}"
    "${SIM_SRC_DIR}"
)
if(MSVC)
    target_compile_options(device_sim_tests PRIVATE /utf-8)
endif()
target_link_libraries(device_sim_tests PRIVATE device_sim_backend_c gtest_main)
add_test(NAME device_sim_tests COMMAND device_sim_tests)
