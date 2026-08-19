# SubStage：PC端监听生命周期

- 所属工作线：PC端
- 所属 Stage：P1 - 监听生命周期
- 依赖前置：无
- 并行状态：需独占修改 `host_app.*`、`host_tcp_server.*`
- 所属阶段：阶段三 - 开发

## 1. 任务目标

保证TCP listener先于服务通告就绪，任一关键启动失败立即回滚，不依赖对象最终析构。

## 2. 接口契约

- `HostTcpServer::Stop()` 幂等。
- `HostTcpServer::Started()` 反映listener是否存在。
- `HostApp::Start()` 顺序：TCP -> announcer -> mDNS兼容注册。
- TCP失败不启动announcer。
- announcer失败关闭listener。
- RequestStop发送bye后关闭announcer和TCP。
- ForceCrash不发送bye但关闭所有资源。

## 3. 文件范围

- `demo/pc/core/host_app.cpp/.h`
- `demo/pc/core/host_tcp_server.cpp/.h`
- `demo/tests/test_host_app.cpp` 或相邻PC生命周期测试

## 4. 测试

- fake backend记录调用顺序。
- TCP失败路径。
- announcer失败回滚。
- Stop幂等。
- 正常模拟Host E2E回归。

## 5. 禁止事项

- 不修改设备端。
- 不修改协议字段。
- 不把mDNS失败作为组播主路径启动失败。

## 6. 完成报告内容

- 启动和停止顺序。
- 回滚测试证据。
- 当前平台未执行项。
