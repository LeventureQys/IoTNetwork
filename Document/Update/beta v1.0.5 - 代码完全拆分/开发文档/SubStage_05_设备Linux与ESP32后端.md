# SubStage 05：设备 Linux 与 ESP32 后端

- 所属 Stage：Stage 2
- 依赖前置：SubStage 01；最终接入依赖 SubStage 03
- 并行状态：可与 SubStage 02、03、04 主体并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

将 Linux 热点/STA 和 ESP32-C2 骨架完整归入设备自包含目录，形成纯 C、职责明确的后端和 ESP-IDF 工程入口，不虚构硬件完成度。

## 2. 当前状态

- `demo/net_linux/` 使用纯 C，但热点和 WiFi 含进程级全局状态。
- `linux_wifi.c` 通过 shell 命令调用 `nmcli`。
- `demo/net_esp32c2/esp32c2_backend.c` 大部分接口为占位错误，未进入 ESP-IDF 工程。

## 3. 文件所有权

本任务独占：

- `demo/device/backends/linux/**`
- `demo/device/backends/esp32c2/**`
- `demo/device/firmware/esp32c2/**`
- `demo/device/config/device_linux.json`
- `demo/device/config/linux_hotspot.json`
- `demo/device/tests/linux/**`
- `demo/device/tests/esp32/**`

不得修改 PC、sim、runtime、UI 或 integration。

## 4. Linux 契约

- 建立实例化 `linux_backend_t`，组合 POSIX socket、`linux_wifi_t`、`linux_hotspot_t` 和文件 NVS，并严格实现设计文档第 8.2.1 节 `device_linux_backend_create` 契约。
- 所有函数首参数为实例，移除 `g_cfg/g_plan/g_runtime/g_sta_iface`。
- 保留热点地址规划、锁、readiness、fail-closed 和逆序 rollback。
- 非 Linux 平台不编译真实 `device_backend_linux` 及任何热点源码；只编译独立 `device_backend_linux_unavailable`，其中仅提供 `device_linux_backend_create` stub。该 stub 必须清零输出并返回 `DEVICE_ERR_NOT_SUPPORTED`，不得返回成功。
- 真实后端初始化失败传播到 facade，绝不回退 sim。
- 外部命令使用 argv + fork/exec 或等价安全 API，不拼接 shell 字符串；日志不得记录密码。

## 5. ESP32 契约

- 将骨架复制到 `backends/esp32c2`，保持完整 `net_backend_t` 布局。
- 未实现接口返回明确 NOT_SUPPORTED/DEMO_ERR；README 列出逐接口状态。
- 建立 `firmware/esp32c2` ESP-IDF 工程，`app_main.c` 为纯 C；只引用设备目录内部 `src/core/common` 和 ESP32 后端。
- 提供 `idf.py set-target esp32c2`、`idf.py build` 文档和配置。
- 宿主构建可提供 C 语法/接口完整性目标，但不得将其报告为 ESP-IDF build 通过。

## 6. 测试

- 迁移 `test_linux_hotspot_config.cpp`、`test_linux_hotspot_runtime.cpp`，fake ops 在普通 Linux 自动运行。
- 新增多实例锁、部分初始化销毁、参数特殊字符、命令 argv、非 root、缺依赖和禁止回退 sim。
- ESP32 工具链存在时实际 `idf.py build`；缺失记录阻塞。flash、SoftAP、DHCP、STA、NVS 和会话为信息性硬件现状检查，不属于本版本通过条件；不得因骨架未实现这些能力而擅自扩展任务范围。

## 7. 输出契约

向 SS06 提供 `device_backend_linux` 条件目标、ESP32 宿主检查目标和纯 C factory 头；向 SS08 提供更新后的后端 README。

## 8. 禁止事项

- 禁止把 Linux 热点复制到 PC。
- 禁止失败时静默切换 sim。
- 禁止将“编译骨架”描述为“硬件功能可用”。
- 禁止在设备后端目录引入 C++。
