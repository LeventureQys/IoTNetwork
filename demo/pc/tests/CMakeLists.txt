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
    test_events.cpp
    test_wifi_result_reason.cpp
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
