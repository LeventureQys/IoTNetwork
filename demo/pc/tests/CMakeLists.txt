# PC 端单元测试（gtest）。第三方 googletest 不继承 PC 的 /WX（目标级配置）。
set(PC_TESTS_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/pc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../core"
    "${CMAKE_CURRENT_SOURCE_DIR}/../backends/sim"
    "${CMAKE_CURRENT_SOURCE_DIR}/../backends/windows"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cjson"
)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/googletest-1.15.2"
                 "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)

add_executable(pc_unit_tests
    test_frame.cpp
    test_protocol.cpp
    test_log.cpp
    test_net_abstraction.cpp
    test_params.cpp
    test_registry.cpp
    test_host_lifecycle.cpp
    test_sim_world.cpp
    test_sim_tcp_endpoint.cpp
    test_sim_socket.cpp
    test_host_app.cpp
    test_host_session.cpp
    test_win_hotspot.cpp
    test_events.cpp
    test_scenario.cpp
    test_paths.cpp
    test_catalog.cpp
    ../app/scenario_runner.cpp
)
target_include_directories(pc_unit_tests PRIVATE ${PC_TESTS_INCLUDE_DIRS})
target_link_libraries(pc_unit_tests PRIVATE
    pc_core
    pc_sim_backend
    pc_common
    gtest_main
)
if(WIN32)
    target_link_libraries(pc_unit_tests PRIVATE pc_win_backend wlanapi iphlpapi ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(pc_unit_tests PRIVATE Threads::Threads)
endif()
pc_enable_warnings(pc_unit_tests)
pc_set_output_dirs(pc_unit_tests)

add_test(NAME pc_unit_tests
         COMMAND pc_unit_tests
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# SS2.2：PC UI 无旧配网文案 + 六节点流程图（独立目标，链接 pc_ui + Qt6::Widgets）。
add_executable(pc_ui_tests
    test_host_window_ui.cpp
)
target_include_directories(pc_ui_tests PRIVATE ${PC_TESTS_INCLUDE_DIRS})
target_link_libraries(pc_ui_tests PRIVATE pc_ui gtest_main)
pc_enable_warnings(pc_ui_tests)
pc_set_output_dirs(pc_ui_tests)

add_test(NAME pc_ui_tests
         COMMAND pc_ui_tests
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
# 无头运行 Qt：指定 offscreen 平台与插件目录（由 Qt6_DIR 推导 Qt 安装前缀）。
get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
set_tests_properties(pc_ui_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_qt_prefix}/plugins")

# SS1.1：跨端契约冻结的配置/协议/contract 单元测试（独立目标，仅链接 pc_common）。
add_executable(pc_contract_tests
    test_protocol.cpp
    test_params.cpp
    test_contract.cpp
)
target_include_directories(pc_contract_tests PRIVATE ${PC_TESTS_INCLUDE_DIRS})
target_compile_definitions(pc_contract_tests PRIVATE
    PC_CONTRACT_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/../../integration_tests/contracts/protocol-contract.golden.json")
target_link_libraries(pc_contract_tests PRIVATE pc_common gtest_main)
pc_enable_warnings(pc_contract_tests)
pc_set_output_dirs(pc_contract_tests)

add_test(NAME pc_contract_tests
         COMMAND pc_contract_tests
         WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
