# SubStage 04：设备模拟后端纯 C 化

- 所属 Stage：Stage 2
- 依赖前置：SubStage 01；最终接入依赖 SubStage 03 提供的接口
- 并行状态：可与 SubStage 02、03、05 主体并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

将设备使用的 `net_sim` 从 C++ 单体重写为设备目录内的纯 C、实例化、可测试模拟后端，保持现有模拟协议和跨进程行为。

## 2. 当前状态

- `demo/net_sim/sim_backend.cpp` 混合 socket、NVS、AP catalog、Linux 真实后端和故障注入。
- `sim_world.cpp` 使用 singleton 与 C++ 容器/锁。
- `sim_tcp_endpoint.cpp` 本质为纯函数但使用 `.cpp`。
- 当前 AP catalog 从 `nvs_dir` 隐式推导。

## 3. 文件所有权

本任务独占：

- `demo/device/backends/sim/**`
- `demo/device/tests/sim/**`

不得修改 `device/src/**`、`device/backends/linux|esp32c2/**`、UI、PC 或 integration。

## 4. 模块和接口

建立纯 C：

- `sim_backend.c`：上下文、vtable、创建/销毁和子模块装配
- `sim_world.c`：目标 WiFi/AP/RSSI/mDNS/组播状态，显式实例
- `sim_socket.c`：TCP/UDP 与平台错误映射
- `sim_tcp_endpoint.c`：模拟地址到 loopback 的纯函数
- `sim_nvs.c`：JSON blob、base64、原子替换
- `sim_ap_catalog.c`：跨进程 AP 发布/读取/删除
- `sim_mdns.c`、`sim_fault.c`、`sim_random.c`

工厂严格实现设计文档第 8.2.1 节 `device_sim_backend_create`、`device_backend_instance_t` 契约；失败时返回明确错误并逆序释放。不得使用 `void *` 无错误创建 API。

## 5. 行为契约

- `sim_catalog_dir` 必须是显式绝对路径，由配置/CLI 传入。
- AP catalog 严格实现设计文档第 8.5 节冻结的文件名、JSON 字段、原子发布、删除和时效契约，不得另行定义 schema。
- TCP endpoint 维持现有模拟地址映射和真实地址不翻译语义。
- NVS 键、文件内容兼容现有模拟数据；非法 base64 必须失败，缓冲不足不得截断。
- 文件写入采用同目录临时文件 + flush + 原子替换；失败清理临时文件。
- 故障 action 保持现有测试所需名称；未知 action 返回 NOT_SUPPORTED/DEMO_ERR。
- 本后端不得包含 Linux hotspot/wifi 真实实现或运行时人格切换。

## 6. 测试

针对纯 C API 重写现有：

- `test_sim_world.cpp`
- `test_sim_socket.cpp`
- `test_sim_tcp_endpoint.cpp`
- `test_sim_nvs.cpp`

增加非法 base64、缓冲不足、目录不可写、损坏 catalog、并发发布/读取、创建中途失败回滚、socket timeout、两个实例隔离和销毁幂等。测试可用 C++ runner，但生产文件必须全部以 C11 编译。

## 7. 交付与验收

- SS06 可将 `device_backend_sim` 链入设备 facade。
- 非 UI 语言扫描无 `.cpp`、Qt、`std::`、class/new/delete。
- Windows 与 Linux sim 单测通过。
- 模块 README 引用并解释设计文档第 8.5 节 AP catalog 契约，不得改字段或时效。

## 8. 禁止事项

- 禁止逐行翻译后保留单文件巨型实现。
- 禁止 singleton 或进程级可变全局状态。
- 禁止引用 PC sim 源码或根 `net_sim`。
- 禁止引入 C++ 标准库包装层。
