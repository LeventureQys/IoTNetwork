#ifndef DEMO_DEVICE_LINUX_BACKEND_H
#define DEMO_DEVICE_LINUX_BACKEND_H

/*
 * 设备 Linux 后端纯 C factory 头。
 *
 * 设计文档 §8.2.1 的跨 SubStage 契约由 SS03 在
 * device/include/device_backend_factory.h 统一定义（device_result_t /
 * device_error_t / device_backend_instance_t / device_linux_backend_options_t
 * 以及 device_linux_backend_create 声明）。本头仅作为 Linux 后端的
 * 自包含 factory 头转发该契约，供 backends/linux 源文件、tests/linux 与
 * SS06 使用；不得自行改动签名或重复定义类型。
 *
 * 依赖：device/include（SS03）已落地；若未来需要脱离 device/include
 * 独立编译，可参考设计文档 §8.2.1 在本头内补齐类型（注意 device_result_t
 * 为枚举，两个头不得同时出现在同一翻译单元）。
 */

#include "device_backend_factory.h"

#endif
