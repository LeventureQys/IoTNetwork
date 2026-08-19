cmake_minimum_required(VERSION 3.20)
project(TactileSenseDevice VERSION 1.0.5 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DEVICE_BUILD_TESTS "构建设备端测试（gtest/CTest）" OFF)
option(DEVICE_WARNINGS_AS_ERRORS "设备生产代码警告视为错误（第三方不继承）" OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

if(MSVC)
    # 生产与测试源文件含 UTF-8 中文注释/字符串（与旧 DemoShared.cmake 一致）
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
# 设备生产目标（全部 C11，显式 C 编译器；禁止设备根外 source/include）
# 依赖方向：provision_device(Qt) -> device_ui(Qt) -> device_host_facade(C)
#           -> device_runtime -> device_core -> device_common/device_cjson
#           -> device_backend_*（仅 runtime 链接所选后端）
# ---------------------------------------------------------------------------

# 第三方 cJSON：不继承生产 /WX
add_library(device_cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(device_cjson PUBLIC third_party/cjson)
set_target_properties(device_cjson PROPERTIES LANGUAGE C)

# 设备公共 C：frame/log/net_abstraction/protocol
add_library(device_common STATIC
    src/common/frame.c
    src/common/log.c
    src/common/net_abstraction.c
    src/common/protocol.c
)
target_include_directories(device_common PUBLIC include src/common)
target_link_libraries(device_common PUBLIC device_cjson)

# 平台适配（win32/posix 按平台选源）
if(WIN32)
    set(DEVICE_PLATFORM_SRC src/platform/win32/device_platform_win32.c)
else()
    set(DEVICE_PLATFORM_SRC src/platform/posix/device_platform_posix.c)
endif()
add_library(device_platform STATIC ${DEVICE_PLATFORM_SRC})
target_include_directories(device_platform PUBLIC src/platform)

# 设备业务核心（纯 C，只依赖抽象层与 cJSON）
add_library(device_core STATIC
    src/core/device_app.c
    src/core/device_eventlog.c
    src/core/device_limits.c
    src/core/device_provision.c
    src/core/device_discovery.c
    src/core/device_session.c
    src/core/device_heal.c
)
target_include_directories(device_core PUBLIC include src/core)
target_link_libraries(device_core PUBLIC device_common device_cjson)

# 模拟后端（SS04 纯 C；include 指向 device/include 真实契约头）
add_library(device_backend_sim STATIC
    backends/sim/sim_backend.c
    backends/sim/sim_world.c
    backends/sim/sim_socket.c
    backends/sim/sim_tcp_endpoint.c
    backends/sim/sim_nvs.c
    backends/sim/sim_ap_catalog.c
    backends/sim/sim_mdns.c
    backends/sim/sim_fault.c
    backends/sim/sim_random.c
)
target_include_directories(device_backend_sim PUBLIC include backends/sim)
target_link_libraries(device_backend_sim PUBLIC device_cjson)
if(WIN32)
    target_link_libraries(device_backend_sim PUBLIC ws2_32)
endif()

# Linux 后端（SS05）：Linux 真实实现 / 非 Linux NOT_SUPPORTED stub。
# 预置变量避免其回退到仓库根级共享目录。
set(DEMO_DEVICE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" CACHE INTERNAL "")
set(DEMO_DEVICE_CJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson" CACHE INTERNAL "")
add_subdirectory(backends/linux)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DEVICE_LINUX_BACKEND_TARGET device_backend_linux)
else()
    set(DEVICE_LINUX_BACKEND_TARGET device_backend_linux_unavailable)
endif()

# 运行时层（SS03）：CLI/配置/路径 + 生命周期/线程/队列/事件 + facade 实现 +
# scenario 引擎（SS06）。链接所选后端（backend_factory dispatcher）。
add_library(device_runtime STATIC
    src/config/device_config.c
    src/config/device_path.c
    src/config/host_options.c
    src/runtime/backend_factory.c
    src/runtime/device_cmd_queue.c
    src/runtime/device_log_queue.c
    src/runtime/device_events.c
    src/runtime/device_runner.c
    src/runtime/device_host.c
    src/runtime/scenario_engine.c
)
target_include_directories(device_runtime PUBLIC
    include
    src/runtime
    src/config
    src/platform
)
target_link_libraries(device_runtime PUBLIC
    device_core
    device_common
    device_cjson
    device_platform
    device_backend_sim
    ${DEVICE_LINUX_BACKEND_TARGET}
)
if(WIN32)
    target_link_libraries(device_runtime PUBLIC ws2_32)
endif()

# facade 聚合目标：UI 只依赖此目标与 device_host.h
add_library(device_host_facade INTERFACE)
target_include_directories(device_host_facade INTERFACE include)
target_link_libraries(device_host_facade INTERFACE
    device_runtime
    device_core
    device_common
    device_cjson
    device_platform
    device_backend_sim
    ${DEVICE_LINUX_BACKEND_TARGET}
)

# Qt UI（C++17/AUTOMOC 仅此处与 provision_device）
add_library(device_ui STATIC
    ui/device_window.cpp
    ui/log_model.cpp
    ui/flow_widget.cpp
)
set_target_properties(device_ui PROPERTIES AUTOMOC ON)
target_include_directories(device_ui PUBLIC ui include)
target_link_libraries(device_ui PUBLIC device_host_facade Qt6::Widgets)

add_executable(provision_device app/qt_main.cpp)
set_target_properties(provision_device PROPERTIES AUTOMOC ON)
target_include_directories(provision_device PRIVATE ui)
target_link_libraries(provision_device PRIVATE device_ui Qt6::Widgets)
# 可执行产物统一到 <build>/bin/<config>（冒烟/安装路径稳定）
set_target_properties(provision_device PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# ---------------------------------------------------------------------------
# 警告（DEVICE_WARNINGS_AS_ERRORS）：只作用于设备生产/UI/可执行目标，
# 第三方（device_cjson、googletest）不继承。
# ---------------------------------------------------------------------------
function(device_apply_warnings_as_errors TARGET)
    if(DEVICE_WARNINGS_AS_ERRORS)
        target_compile_options(${TARGET} PRIVATE
            $<$<C_COMPILER_ID:MSVC>:/WX>
            $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Werror>)
    endif()
endfunction()
foreach(_t device_common device_platform device_core device_backend_sim device_runtime
        ${DEVICE_LINUX_BACKEND_TARGET} device_ui provision_device)
    device_apply_warnings_as_errors(${_t})
endforeach()

# ---------------------------------------------------------------------------
# 测试聚合（SS07 无需修改本文件）
# ---------------------------------------------------------------------------
if(DEVICE_BUILD_TESTS)
    enable_testing()
    include(CTest)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# 协议产物与安装（artifact/bin + config + share/{protocol-contract.json,manifest.json}）
# ---------------------------------------------------------------------------
configure_file(cmake/protocol-contract.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/share/protocol-contract.json" @ONLY)

find_package(Git QUIET)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=8 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE DEVICE_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT DEVICE_GIT_REV)
    set(DEVICE_GIT_REV "unknown")
endif()
string(TIMESTAMP DEVICE_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
if(CMAKE_CONFIGURATION_TYPES)
    set(DEVICE_BUILD_TYPE "multi-config")
elseif(CMAKE_BUILD_TYPE)
    set(DEVICE_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
else()
    set(DEVICE_BUILD_TYPE "Debug")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/share/manifest.json"
"{\n"
"  \"product\": \"tactile-device\",\n"
"  \"version\": \"${PROJECT_VERSION}\",\n"
"  \"platform\": \"${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}\",\n"
"  \"build_type\": \"${DEVICE_BUILD_TYPE}\",\n"
"  \"git_revision\": \"${DEVICE_GIT_REV}\",\n"
"  \"build_time\": \"${DEVICE_BUILD_TIME}\",\n"
"  \"executable\": \"bin/provision_device${CMAKE_EXECUTABLE_SUFFIX}\",\n"
"  \"protocol_contract\": \"share/protocol-contract.json\"\n"
"}\n")

install(TARGETS provision_device RUNTIME DESTINATION bin)
install(FILES
    config/device_sim.json
    config/device_linux.json
    config/linux_hotspot.json
    DESTINATION config)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/share/protocol-contract.json"
    "${CMAKE_CURRENT_BINARY_DIR}/share/manifest.json"
    DESTINATION share)
