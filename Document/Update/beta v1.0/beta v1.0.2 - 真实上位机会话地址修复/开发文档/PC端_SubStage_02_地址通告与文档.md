# SubStage：PC端地址通告与文档

- 所属工作线：PC端
- 所属 Stage：P2 - 地址通告与日志
- 依赖前置：PC端SubStage 01完成后集成；地址过滤部分可先独立开发
- 并行状态：不得与SubStage 01同时修改 `host_app.cpp`
- 所属阶段：阶段三 - 开发

## 1. 任务目标

准确记录真实listener与通告端点，过滤明显无效的Windows IPv4，并明确真实发现以UDP组播为主。

## 2. 文件范围

- `demo/net_win/win_backend.cpp`
- `demo/pc/app/main.cpp`
- `demo/pc/core/host_announcer.cpp/.h`
- `demo/pc/core/host_mdns.cpp/.h`
- `demo/pc/core/host_app.cpp`（等待SubStage 01）
- `demo/README.md` PC段落
- `demo/协议文档.md` PC通告段落

## 3. 契约

- listener保持 `0.0.0.0:<port>`。
- 通告使用一次性选定的 `params.host_virtual_ip:<port>`。
- 拒绝unspecified、loopback、APIPA。
- announce日志打印IP和TCP端口。
- 不实现真实mDNS、多网卡metric或动态刷新。

## 4. Windows错误处理

- socket各阶段失败记录 `WSAGetLastError()`。
- 失败关闭socket，不设置输出句柄。
- 无有效IPv4时main退出，不回退默认或模拟地址。

## 5. 测试与验收

- 当前环境执行可编译的PC core模拟测试。
- Windows构建、listener、UDP抓包和防火墙列为待Windows验收。
- 文档不得把当前mDNS兼容注册描述为真实局域网mDNS。

## 6. 禁止事项

- 不新增第三方mDNS依赖。
- 不新增网卡选择UI。
- 不修改设备端和sim TCP真实目标修复。

## 7. 完成报告内容

- 地址过滤规则。
- 日志样例。
- Windows待验收清单。
