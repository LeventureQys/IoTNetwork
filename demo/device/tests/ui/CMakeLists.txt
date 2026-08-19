cmake_minimum_required(VERSION 3.20)
project(device_ui_tests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设备 Qt 外壳测试：编译生产 UI 源码（device_window/log_model/flow_widget）
# 与 facade fake（不链接真实 device_host.c，仅 device_host.h 声明）。
find_package(Qt6 REQUIRED COMPONENTS Widgets Test)

set(DEVICE_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ui")
set(DEVICE_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")

add_executable(device_ui_tests
    test_device_window.cpp
    fake_host.c
    "${DEVICE_UI_DIR}/device_window.cpp"
    "${DEVICE_UI_DIR}/log_model.cpp"
    "${DEVICE_UI_DIR}/flow_widget.cpp"
)
set_target_properties(device_ui_tests PROPERTIES AUTOMOC ON)
target_include_directories(device_ui_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${DEVICE_UI_DIR}"
    "${DEVICE_INC_DIR}"
)
if(MSVC)
    target_compile_options(device_ui_tests PRIVATE /utf-8)
endif()
target_link_libraries(device_ui_tests PRIVATE Qt6::Widgets Qt6::Test)

add_test(NAME device_ui_tests COMMAND device_ui_tests)
# Qt DLL 目录注入 PATH（Windows 上运行测试需要），并强制 offscreen 平台
get_filename_component(QT_PREFIX_DIR "${Qt6_DIR}/../../.." ABSOLUTE)
set_tests_properties(device_ui_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT_PREFIX_DIR}/bin")
