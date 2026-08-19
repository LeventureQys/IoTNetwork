# SubStage：设备端 TCP 地址与测试

- 所属工作线：设备端
- 所属 Stage：D1 - TCP地址修复
- 依赖前置：无
- 并行状态：可独立执行；不得与其他任务同时修改 `demo/net_sim/sim_backend.cpp`
- 所属阶段：阶段三 - 开发

## 1. 任务目标

修复 Linux真实设备将发现地址改写为loopback的问题，并通过纯逻辑和现有socket测试证明真实、模拟两种语义正确。

## 2. 必读契约

- `net_addr_t.ip`、`port` 均为网络字节序。
- 真实Linux设备模式：`__linux__ && dev_index()>=0 && use_real_wifi_sta`。
- 真实输出等于请求端点，不做二次 `htonl/htons`。
- 模拟AP、模拟Host和普通loopback行为保持。

## 3. 输入与输出

- 输入：运行模式、请求 `net_addr_t`、模拟AP匹配与实际端口、模拟Host地址和端口。
- 输出：最终IPv4/端口，均为网络字节序。
- 错误：helper为纯函数；socket错误继续由 `TcpConnect()` 返回现有 `DEMO_ERR*`。

## 4. 实现要求

- 在 `demo/net_sim/` 新增内部endpoint helper，或提供等价的可独立测试内部接口。
- `TcpConnect()` 使用helper结果构造 `sockaddr_in`。
- 添加请求/实际端点及失败阶段日志。
- 不重构socket发送、接收或UDP代码。

## 5. 测试要求

- 真实 `192.168.1.191:5935` 保持。
- 模拟AP翻译保持。
- 模拟Host翻译保持。
- 普通loopback测试保持。
- 连接未监听端口仍返回超时类错误。

## 6. 禁止事项

- 不修改PC目录和Windows后端。
- 不初始化真实WiFi来完成单测。
- 不硬编码现场上位机IP。
- 不修改公开网络抽象签名。

## 7. 完成报告内容

- 修改文件。
- helper最终签名和字节序说明。
- 执行的测试命令与结果。
- 未执行项和原因。
