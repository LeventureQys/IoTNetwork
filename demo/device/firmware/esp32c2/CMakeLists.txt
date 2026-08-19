# ESP-IDF 工程入口：ModuTech 设备（ESP32-C2 骨架）
#
# 自包含约束：本工程只引用 demo/device 目录内部的：
#   - backends/esp32c2/    （ESP32-C2 网络后端骨架，纯 C）
#   - include/             （SS03 落地的设备公开纯 C 头：net_abstraction 等）
#   - src/common/、src/core/（SS03 落地的平台无关 runtime 与核心）
# 不得引用仓库根目录、PC 目录或任何 C++/Qt 代码。
#
# 注意：src/common / src/core / include 由 SS03 迁移落地；在它们落地之前，
# 本工程无法配置/构建（预期阻塞，见 README）。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(modutech_device_esp32c2)
