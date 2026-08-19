# beta v1.0.3 PC 端修改计划

## SubStage: PC 端配网收尾与结果日志

- 所属 Stage：Stage 2
- 依赖前置：无
- 并行状态：可与设备端修改并行
- 所属阶段：阶段三 - 开发任务书

## 1. 任务目标

让 PC 准确处理 `close_ap` 的发送结果，并在收到 `wifi_result ok` 后可靠结束配网连接、切回目标 WiFi，等待设备通过 UDP/TCP 回连。

## 2. 修改文件

### 必须修改

| 文件 | 修改内容 |
|---|---|
| `demo/pc/core/host_provision_client.cpp` | 检查 close_ap 发送结果，调整配网阶段和切网日志 |
| `demo/tests/test_host_app.cpp` 或新增 PC 配网测试文件 | 覆盖 close_ap 正常与失败行为 |

### 按需要修改

| 文件 | 条件 |
|---|---|
| `demo/pc/core/host_provision_client.h` | 仅当测试需要暴露小型结果类型或辅助接口时修改 |
| `demo/net_sim/sim_backend.cpp` | 仅当现有注入机制无法模拟 close_ap 发送失败时，增加最小故障注入 |
| `demo/tests/CMakeLists.txt` | 仅在新增测试文件时登记 |
| `demo/README.md`、`demo/协议文档.md` | 开发完成时同步 close_ap 最佳努力语义 |

## 3. 发送接口契约

当前本地 `send_frame()` 返回底层 `net_sock_send()` 返回值。修改为：

```text
完整写入整帧：DEMO_OK
底层负错误码：原样返回
返回非负但小于帧长：DEMO_ERR
frame_wrap 失败：原错误码
```

本版本不实现循环重试。这样可让 close_ap 日志准确，但不扩大为公共 TCP 发送重构。

现有 auth 和 wifi_config 调用方可继续沿原流程，但建议同步检查发送返回值：

- auth 写入失败：结束本次配网并记录失败。
- wifi_config 写入失败：结束本次配网并记录失败。
- close_ap 写入失败：记录警告，但配网 WiFi 配置结果仍视为已被设备接受。

## 4. ProvisionDevice 行为

### 4.1 收到 wifi_result ok

立即记录：

```text
设备已接受目标 WiFi 配置：设备热点=<AP>，目标SSID=<SSID>，设备IP=<IP>，等待设备回连
```

该日志只表示阶段二成功，不表示业务 TCP 已建立。

### 4.2 发送 close_ap

构造现有 JSON：

```json
{"cmd":"close_ap"}
```

调用修改后的 `send_frame()`：

- `DEMO_OK`：记录 `配网：close_ap 已完整写入配网连接`。
- 非 `DEMO_OK`：记录 `配网：close_ap 写入失败，返回码=%d，设备将自动完成交接`。

两种结果都允许 `ProvisionDevice()` 返回 WiFi 配置成功，因为设备端具备自动交接。不得因 close_ap 失败无限重试或停留在设备热点。

### 4.3 结束配网连接

保持顺序：

1. 关闭配网 TCP。
2. 记录“正在断开设备热点”。
3. 调用 `net_wifi_sta_disconnect()`。
4. 真实 WiFi 模式下记录“正在切回目标 WiFi”。
5. 调用 `net_wifi_sta_connect()`。
6. 成功时记录“已切回目标 WiFi”；失败时记录原因码并提示手动连接。

### 4.4 完整成功语义

`HostProvisionClient::ProvisionDevice()` 的 `0` 仍表示设备接受 WiFi 配置，不修改公开签名。

业务会话是否成功继续由 `HostApp`/`HostTcpServer` 的设备在线状态判断。不要在 `ProvisionDevice()` 中等待 `device_hello`，避免阻塞配网串行流程和扩大类职责。

## 5. 测试设计

### 5.1 正常 E2E 回归

既有：

```text
HostE2E.ProvisionRegisterHeartbeat
```

必须继续通过，证明正常 close_ap 路径没有回归。

### 5.2 close_ap 失败测试

优先采用最小故障注入：让下一次或指定命令对应的 socket send 返回错误。

测试目标：

1. auth 和 wifi_config 正常。
2. wifi_result ok 正常返回。
3. close_ap 发送失败。
4. `ProvisionDevice()` 仍结束配网流程，不死循环。
5. 配合设备端自动交接后，设备最终上线。

建议测试名称：

```text
HostE2E.CloseApSendFailureStillRegisters
```

如果精确到命令的注入会显著扩大模拟后端，不强制在 PC 单测中注入；可以由设备端“不发送 close_ap”测试覆盖核心容错，PC 端至少对 `send_frame()` 的完整写判定增加纯逻辑测试或可控 socket 测试。

### 5.3 wifi_result fail 回归

验证设备返回 fail 时：

- PC 不发送 close_ap。
- PC 返回配网失败。
- 日志保留具体 reason。

## 6. 禁止事项

- 不新增 `close_ap_ack`。
- 不增加无限重试或长时间等待。
- 不在 `ProvisionDevice()` 中等待 device_hello。
- 不修改 UDP host_announce JSON。
- 不实现多网卡选择或真实 mDNS。
- 不修改设备状态机源码。
- 不把 close_ap 失败记录为整个 WiFi 配置失败。

## 7. 完成报告要求

PC 端完成后单独提交：

- 修改文件列表。
- close_ap 成功与失败日志样例。
- 测试名称与结果。
- PC 工程构建结果。
- 模拟 E2E 结果。
- 真实测试时提供切回目标 WiFi、host_announce 和 device_hello 的连续 PC 日志。
