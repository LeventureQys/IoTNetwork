# SubStage 07：端侧测试与跨端 Integration

- 所属 Stage：Stage 3
- 依赖前置：SubStage 02、06
- 并行状态：需等待两端公开产物和 CLI/事件契约
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

完成测试所有权迁移，建立不包含任何端侧生产源码的 `integration_tests`，恢复和增强现有跨端回归能力。

## 2. 当前状态

- `demo/tests/CMakeLists.txt` 同时编译双方核心。
- `test_host_app.cpp` 包含双方私有头并使用同进程 `SimWorld`。
- `run_two_gui.ps1` 不采集退出码、日志、超时或残留进程。

## 3. 文件所有权

本任务独占 `demo/integration_tests/**`，但不得改 SS01 golden 的值。PC 测试与注册由 SS02 完成；设备测试分别由 SS03/04/05 创建并由 SS06 的 `tests/CMakeLists.txt` 聚合。本任务只检查端侧 CTest 可执行性，不修改两端测试或顶层 CMake；生产或端测缺陷报告 MainAgent 回派原 SubStage。

## 4. 测试归属

检查并执行设计文档第 12 节既定归属，不再搬动端侧文件：

- PC：registry、host lifecycle、PC params、PC fake app、PC sim/socket/common。
- 设备：device logic/sm/backoff/provision、设备 config/runtime/sim/Linux/common。
- integration：原 `test_host_app.cpp` 的首次配网、bye、重连、close_ap fail、wifi_result fail、双向 app_data。

`test_provision.cpp` 归设备 TCP 契约测试，不编译 PC 核心。

## 5. Integration 边界

CMake/runner 只接收：`PC_EXECUTABLE`、`DEVICE_EXECUTABLE`、两个 artifact 根、runtime root 和 timeout。禁止 `add_subdirectory(../pc|../device)`、链接端侧库、include 私有头或编译端侧 `.c/.cpp`。

runner 为每个场景创建独立绝对目录和两份配置，共享显式 `sim_catalog_dir`，启动两个不同 PID，读取 JSONL 和退出码，超时终止并保留证据。

## 6. 必测场景

1. protocol manifest 与 golden 语义相等。
2. 首次配网→注册→至少两次心跳。
3. PC 优雅退出/host_bye，端口和进程清理。
4. 公开 scenario 注入断链→离线→重连上线。
5. close_ap 单次发送失败但最终上线。
6. wifi_result fail 时不发送 close_ap，设备继续配网。
7. PC→设备和设备→PC app_data：每个 scenario action 必须匹配唯一 `scenario_result`；覆盖 512 成功、513 以 `payload_too_large` 拒绝和 UTF-8 字节长度。
8. 双向帧边界与 proto version 拒绝。

不得通过匹配中文自由日志作为唯一断言；使用 JSONL 稳定事件。

## 7. 自包含验收 runner

增加 PC、设备分别复制到系统临时目录的脚本；复制后配置、构建、CTest、offscreen 启动。扫描构建日志和 compile commands，不能出现原仓库路径。另建立只复制 artifacts + integration runner 的二进制隔离测试。

## 8. 错误处理

- 任一进程异常退出、事件缺失、manifest 不一致、超时或残留进程均失败。
- runner 自身异常必须尽力终止子进程并保留日志。
- 外部真实硬件缺失只影响真实平台项，不影响 sim integration。

## 9. 禁止事项

- 禁止把协议生产实现复制进 integration。
- 禁止恢复同进程双端 fixture。
- 禁止用旧构建缓存或历史报告代替当前执行。
- 禁止为了测试方便公开核心私有结构。
