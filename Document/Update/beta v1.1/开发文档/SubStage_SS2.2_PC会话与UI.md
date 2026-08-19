# SubStage SS2.2：PC 一对一会话与 UI

## SubStage: PC 一对一会话与 UI
- 所属 Stage：Stage 2 - 端侧实现
- 依赖前置：SS1.1、SS2.1 的 HostApp 新生命周期
- 并行状态：需等待 SS2.1；可与设备端工作并行
- 所属阶段：阶段三 - 开发

## 1. 目标

将 PC 多设备/配网 UI 裁剪成一个热点、一个设备、一个 TCP 会话；同步简化握手报文和流程图。

## 2. 当前代码

- 多连接容量：`demo/pc/core/host_tcp_server.cpp:118`。
- 重复在线连接替换旧连接：`demo/pc/core/host_tcp_server.cpp:278`。
- hello 要求 type/capabilities：`demo/pc/core/host_tcp_server.cpp:249`。
- ack 含 server_time/fw_min_req：`demo/pc/core/host_tcp_server.cpp:306`。
- UI 仍下发 WiFi：`demo/pc/ui/host_window.cpp:49`、`demo/pc/ui/host_window.cpp:150`。

## 3. 文件所有权

允许修改 `core/host_tcp_server.*`、必要的 registry、`ui/host_window.*`、`ui/components/handshake_flow_widget.*`、PC 会话/UI测试和 README。若 SS2.1 已修改 `host_app.*`，只做调用适配，不重写其生命周期。

禁止修改 Windows/sim backend、设备端、contract 值、integration runner。

## 4. 会话契约

- 同时最多一个 pending 或 online socket。
- 第二连接回复：`{"cmd":"host_ack","status":"busy","reason":"single_device_only"}`，关闭并发出 `single_device_rejected`。
- 在线同 ID 新连接也拒绝，不替换旧连接。
- 离线同 ID可以恢复，session_id 匹配则复用。
- hello 必需字段：id/fw_version/proto_ver/uptime；session_id 可选。
- ack ok：status/heartbeat_interval/session_id/proto_ver。
- ack fail 版本错误：reason=`unsupported_protocol`。
- pong 必须带与 ping 相同的 seq。

保留 frame、malformed、heartbeat、app_data 逻辑。

## 5. UI

删除目标 WiFi、密码、扫描、配网按钮和回调。热点区只读显示 `pc_ap_ssid`、`192.168.137.1/24`、5935、运行状态。表最多一行。停止文案“停止服务并关闭热点”。流程节点：启动、热点启动、固定 IP、TCP 监听、设备握手、会话在线。

## 6. 错误处理

- 非法 hello 计 malformed；达到阈值关闭。
- proto 不符回复 fail 后关闭。
- busy 不影响现有在线连接。
- app_data 超 512 返回 `DEMO_ERR`；UI 提示。

## 7. 测试

1. 首个合法 hello→ack ok。
2. hello 缺必需字段。
3. proto=2→unsupported_protocol。
4. 第二连接→busy/single_device_only，首连接在线。
5. 同 ID 在线重连不替换旧 socket。
6. 离线恢复。
7. ping seq 回显。
8. 心跳超时。
9. app_data 512成功、513拒绝、UTF-8字节计数。
10. QtTest 验证不再出现“下发 WiFi/设备 AP/mDNS/host_announce”。

## 8. 验收标准

- PC core 无动态 host_max_conn 业务分支，固定一对一。
- UI 无旧配网控件。
- handshake_flow 只展示六个新节点。
- 单元测试通过。

## 9. 禁止事项

不修改 SS2.1 backend；不恢复多设备；不删除 frame 健壮性；不泄露密码；不修改设备端。

## 10. 完成报告

提供修改文件、协议处理差异、测试命令与结果。