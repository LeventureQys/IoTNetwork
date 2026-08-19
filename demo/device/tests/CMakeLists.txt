cmake_minimum_required(VERSION 3.20)
project(device_tests LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 测试聚合入口（SS06 建立）：只 add_subdirectory 端侧测试子目录，
# 后续 Stage 无需修改设备顶层 CMakeLists.txt。
# - runtime/config/core：SS03 设备运行时/配置/核心契约测试
# - sim：SS04 模拟后端纯 C 测试
# - linux：SS05 Linux 后端（非 Linux 仅 stub 检查，可配置不失败）
# - esp32：SS05 ESP32 骨架宿主检查
# - ui：SS06 设备 Qt 外壳测试（facade fake，offscreen）
enable_testing()
include(CTest)

# googletest 统一添加一次（各测试子目录仅在独立构建时自行添加，见各自 CMakeLists）；
# 测试依赖不进入安装产物。
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/googletest-1.15.2"
                 "${CMAKE_CURRENT_BINARY_DIR}/googletest")

add_subdirectory(config)
add_subdirectory(core)
add_subdirectory(runtime)
add_subdirectory(sim)
add_subdirectory(linux)
add_subdirectory(esp32)
add_subdirectory(ui)
