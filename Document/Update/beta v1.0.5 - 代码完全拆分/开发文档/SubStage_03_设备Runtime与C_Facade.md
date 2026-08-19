# SubStage 03：设备 Runtime 与 C Facade

- 所属 Stage：Stage 2
- 依赖前置：SubStage 01
- 并行状态：可与 SubStage 02、04、05 并行；SubStage 06 需等待本任务
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

在 `demo/device/` 内建立纯 C 公共层、配置、平台线程、runtime 和 UI 唯一可用的 C facade。解决当前 `main.cpp` 过重、UI 直连核心和跨线程数据竞争。

## 2. 当前状态

- 设备核心位于 `demo/device/device_*.c/.h`，业务主体为 C。
- `demo/device/device_app.h:11` 暴露核心对象和内部函数给 UI。
- `demo/device/device_priv.h` 中 stop、快照和 UI TX 字段跨线程无可靠同步。
- `demo/common/net_abstraction.c:18` 忽略 backend init 错误。

## 3. 文件所有权

本任务独占：

- `demo/device/include/**`
- `demo/device/src/common/**`
- `demo/device/src/config/**`
- `demo/device/src/runtime/**`
- `demo/device/src/platform/**`
- `demo/device/src/core/**`
- `demo/device/third_party/cjson/**`
- `demo/device/tests/runtime/**`
- `demo/device/tests/config/**`
- `demo/device/tests/core/**`

本任务不得修改 `demo/device/backends/**`（SS04/05）、`app/**`、`ui/**`、最终设备 `CMakeLists.txt`（SS06）。可以提供供 SS04/05/06 使用的纯 C 头。

## 4. 公开接口

严格实现 `设计文档.md` 第 8.2～8.4 节的 facade 与 `device_backend_factory.h`：

- `device_result_t`、`device_error_t`
- `device_host_options_t`
- `device_snapshot_t`、`device_log_record_t`
- `device_host_create/start/request_stop/join/destroy`
- snapshot、app_data、fault、log drain

UI 只需包含 `device_host.h`。本任务按设计文档第 8.2.1 节实现宿主 dispatcher：SIM 调 SS04，LINUX 调 SS05，宿主 ESP32C2 明确返回 NOT_SUPPORTED；不得把具体后端业务实现写入 facade。

## 5. 生命周期与并发

- 固定容量 32 命令队列；满时 BUSY，不覆盖。
- 日志队列使用 pull 模式并报告 dropped。
- 快照完整复制并同步保护。
- stop 幂等、join 可超时、运行中 destroy 返回 BUSY。
- duration 由 C runner 的单调时钟实现，不由 Qt 定时器实现。
- signal bridge 只设置安全标志或事件。
- join 成功后不得再更新快照或日志。

## 6. 配置和路径

将共享 `demo_params_t` 拆为设备业务、宿主、sim、Linux 配置；遵循设计文档第 10 节兼容加载与显式路径规则。不得 `chdir` 或向上搜索；保持历史缺失文件默认值、字段容错和截断语义。`fresh` 只能删除解析出的本设备 NVS 文件。

## 7. 核心迁移

将现有 `device_*.c/.h` 移入 `src/core`，保持配网、发现、会话、自愈、eventlog、限速行为。移除核心中的 UI TX 槽、非原子 stop 和 UI getter；改由 runtime 命令和 snapshot 适配。核心私有头不得安装给 UI。

统一修改设备副本的 `net_ctx_create` 为设计文档第 9 节契约；错误码保持 `DEMO_*` 现有语义，facade 在边界映射为 `DEVICE_*`。

## 8. 测试

新增设备内部测试：

- create/start/stop/join/destroy 全状态矩阵
- init 失败回滚、线程创建失败、join timeout
- 命令队列满、停止后提交、512/513 字节
- 快照并发、日志溢出和 join 后无写入
- 配置文件缺失/非法/类型/范围/超长/路径解析
- 不改变 CWD，fresh 不越权删除

可使用 C++ GoogleTest 作为测试驱动，但被测生产目标必须由 C 编译器编译且不依赖 C++ runtime/Qt。

## 9. 输出契约

SS04/05 获得纯 C backend factory/error/platform 接口；SS06 获得 `device_host.h` 和可链接 C targets。所有头必须自足、有 `extern "C"` 保护且不泄漏 cJSON 或核心私有结构。

## 10. 禁止事项

- 禁止修改 PC、integration、设备 UI 和后端实现目录。
- 禁止在 `src/` 中新增 C++ 或 Qt。
- 禁止让 UI 继续持有 `device_app_t`/`net_ctx_t`。
- 禁止通过 volatile 或注释替代同步原语。
