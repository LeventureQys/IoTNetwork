# SubStage 01：协议契约与迁移基线

- 所属 Stage：Stage 1
- 依赖前置：无依赖
- 并行状态：需先完成；后续所有 SubStage 依赖本产物
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

本任务冻结拆分前的协议、持久化和文件迁移基线，为 PC、设备和集成测试提供无源码共享的兼容契约。不得修改任何线上行为。

## 2. 当前状态

- 协议常量位于 `demo/include/protocol.h:9`。
- 帧格式实现位于 `demo/common/frame.c`。
- 持久化键可从 `demo/device/device_app.c:216`、`demo/device/device_eventlog.c:32` 确认。
- 当前测试以 `demo/tests/test_protocol.cpp` 和 `test_frame.cpp` 为基础。

## 3. 文件所有权

本任务独占新增或修改：

- `demo/integration_tests/contracts/protocol-contract.golden.json`
- `demo/integration_tests/contracts/README.md`
- `Document/Update/beta v1.0.5 - 代码完全拆分/迁移清单.md`

本任务不得修改 PC、设备生产源码或其他 SubStage 任务书。

## 4. 实现要求

### 4.1 Golden contract

建立语义 JSON，至少包含：

- `schema_version=1`
- `protocol_version=1`
- 帧头 2 字节、大端、最大 payload 1024
- TCP 5935
- 组播 `224.0.2.1:5936`
- mDNS `_tactile._tcp`
- app_data 512 字节、session id 8
- 14 个命令名
- WiFi reason 0/201/202/205/500
- 设备状态及固定整数值
- 持久化键 `wifi_creds`、`host_candidates`、`evlog`

不得将 golden 生成为两端生产头文件；它只用于验收。

### 4.2 迁移清单

逐文件记录旧路径、PC 新路径、设备新路径、动作（复制/迁移/拆分/删除）、负责 SubStage 和删除前置。至少覆盖根 `cmake/common/include/net_*/shared_ui/config/tests/third_party` 与两端现有文件。

明确根共享目录只能由 SS08 在 SS02～SS07 全部通过后删除。

## 5. 输入输出

- 输入：现有仓库源文件、`设计文档.md` 第 4、6、12 节。
- 输出：golden contract 和完整迁移清单。
- 使用方：SS02～SS08，路径固定为上述文件。

## 6. 错误处理

- 无法从源码确认的值不得猜测；追加到阶段三问题清单并报告 MainAgent。
- 发现文档与代码冲突时，以当前代码为行为基线，同时记录差异，不自行改变协议。

## 7. 测试与验收

- JSON 可被标准解析器读取。
- commands 无重复，设备状态值唯一。
- 所有值与 `demo/include/protocol.h`、当前 NVS 调用点一致。
- 迁移清单没有“责任 SubStage 为空”的生产文件。

## 8. 禁止事项

- 禁止修改协议版本或常量。
- 禁止创建第三份可编译公共库。
- 禁止提前移动/删除生产源码。
