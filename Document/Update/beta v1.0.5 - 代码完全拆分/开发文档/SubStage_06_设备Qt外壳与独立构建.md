# SubStage 06：设备 Qt 外壳与独立构建

- 所属 Stage：Stage 3
- 依赖前置：SubStage 03、04；接入 Linux/ESP32 目标需 SubStage 05
- 并行状态：需等待依赖
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

把设备 Qt 程序缩成顶层 UI 外壳，并建立完全自包含的设备 CMake、配置、脚本、第三方和协议产物。

## 2. 当前状态

- `demo/device/app/main.cpp` 混合全部生命周期。
- `device_window` 直接持有核心和网络对象。
- `demo/device/CMakeLists.txt` 引用父目录共享源。

## 3. 文件所有权

本任务独占：

- `demo/device/app/**`
- `demo/device/ui/**`
- `demo/device/CMakeLists.txt`
- `demo/device/cmake/**`
- `demo/device/config/device_sim.json`
- `demo/device/scripts/**`
- `demo/device/README.md`
- `demo/device/third_party/googletest-1.15.2/**`
- `demo/device/share/protocol-contract.json` 或其生成规则
- `demo/device/tests/CMakeLists.txt`

不得修改 SS03/04/05 所有目录；接口偏差报告 MainAgent，不私自改依赖文件。

## 4. Qt 外壳

`app/qt_main.cpp` 只做：QApplication、parse options、create/start host、构造窗口、exec、request_stop/join/destroy、退出码映射。不得包含配置、filesystem、thread、backend 或 core 私有头。

`DeviceWindow` 只持有 `device_host_t *`，通过 snapshot/log drain/command/fault/stop 接口工作。日志不再注册 Qt 全局 sink；关闭窗口只请求停止，不自行销毁核心。

## 5. CMake

目标至少为：`device_cjson`、`device_common`、`device_core`、`device_runtime`、平台目标、`device_backend_sim`、Linux 条件目标、`device_host_facade`、`device_ui`、`provision_device` 和测试目标。

- C11 生产目标显式使用 C 编译器。
- C++17/Qt/AUTOMOC 仅作用于 `device_ui` 和 executable。
- 不得出现 `DEMO_ROOT` 或设备根外源码路径。
- 配置和 manifest 复制到 artifact 布局。
- `DEVICE_BUILD_TESTS=ON` 时固定执行 `add_subdirectory(tests)`；`tests/CMakeLists.txt` 只聚合 SS03 的 `runtime/config/core`、SS04 的 `sim`、SS05 的 `linux/esp32` 子目录，后续 SS07 无需修改设备顶层 CMake。
- 提供 `cmake --install <device-build> --config Debug --prefix <device-artifact>` 标准 install 规则。

## 6. CLI 与启动

保留 `--config`、`--device-index`、`--fresh`、`--duration`；新增 `--backend sim|linux`、`--runtime-dir`、`--sim-catalog-dir`、`--log-dir`、`--events-jsonl`、`--scenario`。不传测试参数时无测试通道。

Windows/Linux sim 均支持 Qt offscreen。Linux real 只在 Linux 创建，失败不回退 sim。

## 7. 测试和门禁

- UI 使用 facade fake/test seam 验证窗口启动、快照刷新、日志 drain、命令错误提示和关闭。
- 语言扫描：除 `app/qt_main.cpp` 和 `ui/` 外无 `.cpp`、Qt、`std::`、new/delete/class。
- 构建图：core/runtime/backend 不链接 Qt/C++ UI。
- 将 `demo/device` 单独复制到临时目录后配置、构建、CTest 和 offscreen 启动通过。
- 从任意空 CWD 使用绝对 executable/config 启动，输出只落显式 runtime 目录。

## 8. 禁止事项

- 禁止为解决接口问题让 UI include 私有头。
- 禁止在 Qt 入口恢复业务线程/配置/路径逻辑。
- 禁止引用根 shared_ui、common、include、net_sim 或 third_party。
