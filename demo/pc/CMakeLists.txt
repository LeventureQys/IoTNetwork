cmake_minimum_required(VERSION 3.20)
project(ModuTechPc VERSION 1.0.6 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 仅 UI 目标启用 AUTOMOC
set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_AUTOUIC OFF)

option(PC_BUILD_TESTS "构建 PC 单元测试" OFF)
option(PC_WARNINGS_AS_ERRORS "PC 自有目标警告视为错误（第三方不继承）" ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PcFlags.cmake")
pc_require_toolchain()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ---------------- 第三方 ----------------
add_library(pc_cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(pc_cjson PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson")
pc_set_output_dirs(pc_cjson)

# ---------------- 公共 C ----------------
add_library(pc_common STATIC
    common/frame.c
    common/log.c
    common/net_abstraction.c
    common/params.c
    common/protocol.c
    common/pc_event.c
    common/pc_paths.c
    common/pc_scenario.c
    common/pc_sha256.c
)
target_include_directories(pc_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pc"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson"
)
target_link_libraries(pc_common PUBLIC pc_cjson)
pc_enable_warnings(pc_common)
pc_set_output_dirs(pc_common)

# ---------------- 后端 ----------------
# socket：TCP/UDP/非阻塞 socket/时钟/随机数（不含 WiFi/AP/NVS/SimWorld）
add_library(pc_socket_backend STATIC backends/socket/pc_socket_backend.cpp)
target_include_directories(pc_socket_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pc"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends/socket"
)
target_link_libraries(pc_socket_backend PUBLIC pc_common)
if(WIN32)
    target_link_libraries(pc_socket_backend PUBLIC ws2_32)
endif()
pc_enable_warnings(pc_socket_backend)
pc_set_output_dirs(pc_socket_backend)

# sim：PC 模拟 WiFi/AP catalog/endpoint 翻译
add_library(pc_sim_backend STATIC
    backends/sim/sim_world.cpp
    backends/sim/sim_backend.cpp
    backends/sim/sim_tcp_endpoint.cpp
)
target_include_directories(pc_sim_backend PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pc"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends/sim"
)
target_link_libraries(pc_sim_backend PUBLIC pc_common pc_socket_backend)
if(WIN32)
    target_link_libraries(pc_sim_backend PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(pc_sim_backend PUBLIC Threads::Threads)
endif()
pc_enable_warnings(pc_sim_backend)
pc_set_output_dirs(pc_sim_backend)

# windows：Native WiFi/IP/网关 + 组合 pc_socket_backend；绝不链接 pc_sim_backend
if(WIN32)
    add_library(pc_win_backend STATIC
        backends/windows/win_backend.cpp
        backends/windows/win_hotspot.cpp
    )
    target_include_directories(pc_win_backend PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include/pc"
        "${CMAKE_CURRENT_SOURCE_DIR}/backends/windows"
        "${CMAKE_CURRENT_SOURCE_DIR}/backends/socket"
    )
    target_link_libraries(pc_win_backend PUBLIC
        pc_common pc_socket_backend wlanapi iphlpapi ws2_32 windowsapp runtimeobject
    )
    pc_enable_warnings(pc_win_backend)
    pc_set_output_dirs(pc_win_backend)
endif()

# ---------------- 核心 ----------------
add_library(pc_core STATIC
    core/host_registry.cpp
    core/host_tcp_server.cpp
    core/host_app.cpp
)
target_include_directories(pc_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pc"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
)
target_link_libraries(pc_core PUBLIC pc_common pc_sim_backend)
if(WIN32)
    target_link_libraries(pc_core PUBLIC ws2_32)
endif()
pc_enable_warnings(pc_core)
pc_set_output_dirs(pc_core)

# ---------------- UI（仅本目标 AUTOMOC） ----------------
add_library(pc_ui STATIC
    ui/host_window.cpp
    ui/components/log_model.cpp
    ui/components/handshake_flow_widget.cpp
    ui/components/serial_data_model.cpp
)
set_target_properties(pc_ui PROPERTIES AUTOMOC ON)
target_include_directories(pc_ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/components"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pc"
)
target_link_libraries(pc_ui PUBLIC pc_core Qt6::Widgets)
pc_enable_warnings(pc_ui)
pc_set_output_dirs(pc_ui)

# ---------------- 可执行 ----------------
add_executable(provision_pc app/main.cpp app/scenario_runner.cpp)
target_include_directories(provision_pc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/app"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/pc"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends/sim"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends/windows"
)
target_link_libraries(provision_pc PRIVATE pc_ui pc_core pc_sim_backend pc_common Qt6::Widgets)
if(WIN32)
    target_link_libraries(provision_pc PRIVATE pc_win_backend ws2_32 shell32 advapi32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(provision_pc PRIVATE Threads::Threads)
endif()
pc_enable_warnings(provision_pc)
pc_set_output_dirs(provision_pc)

# 构建后将默认配置复制到可执行目录，避免运行时"配置文件缺失"警告
add_custom_command(TARGET provision_pc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:provision_pc>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/pc_config.json"
            "$<TARGET_FILE_DIR:provision_pc>/config/pc_config.json")

# ---------------- 协议产物与安装 ----------------
include(GNUInstallDirs)

# protocol-contract.json：PC 自有静态副本（语义与 SS01 golden 一致，非从设备目录读取）
install(FILES share/protocol-contract.json DESTINATION share)

# manifest.json：构建时按配置生成（product/version/platform/build_type/git_revision/可执行相对路径）
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE PC_GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PC_GIT_REVISION)
    set(PC_GIT_REVISION "unknown")
endif()
if(WIN32)
    set(PC_PLATFORM "windows")
    set(PC_EXEC_REL "bin/provision_pc.exe")
else()
    set(PC_PLATFORM "linux")
    set(PC_EXEC_REL "bin/provision_pc")
endif()
set(PC_MANIFEST_CONTENT
"{\n  \"product\": \"provision_pc\",\n  \"version\": \"${PROJECT_VERSION}\",\n  \"platform\": \"${PC_PLATFORM}\",\n  \"build_type\": \"$<CONFIG>\",\n  \"git_revision\": \"${PC_GIT_REVISION}\",\n  \"executable\": \"${PC_EXEC_REL}\"\n}\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/share/manifest-$<CONFIG>.json"
     CONTENT "${PC_MANIFEST_CONTENT}")
install(CODE "
    file(COPY \"${CMAKE_CURRENT_BINARY_DIR}/share/manifest-\${CMAKE_INSTALL_CONFIG_NAME}.json\"
         DESTINATION \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/share\")
    file(RENAME \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/share/manifest-\${CMAKE_INSTALL_CONFIG_NAME}.json\"
         \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/share/manifest.json\")
")

install(TARGETS provision_pc RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION config)

# ---------------- 测试 ----------------
if(PC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
