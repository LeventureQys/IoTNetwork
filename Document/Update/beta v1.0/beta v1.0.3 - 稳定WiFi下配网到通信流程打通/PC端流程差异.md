# beta v1.0.3 PC 端流程差异

- 对象：PC 配网客户端、UDP 通告和 TCP 服务
- 当前目标：在稳定目标 WiFi 下完成设备配网并等待设备回连

## 1. PC 端计划流程

```text
选择目标 WiFi
  -> 连接设备热点
  -> TCP auth
  -> 发送 wifi_config
  -> 收到 wifi_result ok
  -> 发送 close_ap
  -> 关闭配网 TCP
  -> 断开设备热点
  -> 切回目标 WiFi
  -> 发送 UDP host_announce
  -> 接收设备 TCP 连接
  -> host_ack
  -> pong
```

## 2. 当前已实现

- 扫描和连接设备热点。
- 配网 TCP 连接。
- auth 和 wifi_config 发送。
- wifi_result 接收。
- close_ap 构造和发送调用。
- 断开设备热点并尝试切回目标 WiFi。
- UDP `host_announce`。
- TCP 服务、`host_ack` 和 pong。

## 3. 当前缺口

### P1. `close_ap` 发送结果没有检查

当前代码调用 `send_frame()` 后，不检查返回值，直接记录“已发送 close_ap”并把配网结果设为成功。

相关代码：

- `demo/pc/core/host_provision_client.cpp:242-250`

这会造成 PC 日志与设备实际接收结果不一致。

### P2. PC 切回目标 WiFi 的结果需要作为流程证据

当前代码已有自动切回逻辑，但真实联调必须区分：

- 已成功切回目标 WiFi；
- 自动切回失败，需要用户手动连接；
- 当前实际 SSID 不是目标 WiFi。

相关代码：`demo/pc/core/host_provision_client.cpp:266-276`。

### P3. 配网成功不能只以 `close_ap` 调用完成判定

本版本中，PC 配网客户端可以在收到 `wifi_result ok` 后结束配网步骤；设备是否进入业务通信，需要通过后续 `device_hello` 判断。

因此 PC 界面或日志应区分：

- “WiFi 配置已下发并被设备接受”；
- “设备已回到目标网络并建立业务会话”。

## 4. 本版本 PC 端修改范围

### 4.1 检查 close_ap 发送结果

- 成功：记录“close_ap 已写入配网连接”。
- 失败：记录返回码和“设备将按自动交接机制继续”。
- 不因 close_ap 失败无限重试或阻塞 PC 切回目标 WiFi。

PC 端不需要增加复杂确认协议，因为设备端会在短宽限期后自主关闭 AP。

### 4.2 明确切网日志

至少记录：

```text
正在断开设备热点
正在切回目标 WiFi <SSID>
已切回目标 WiFi <SSID>
```

失败时记录真实原因，并提示手动连接。

### 4.3 用业务连接确认完整成功

收到设备 `device_hello` 并发送 `host_ack ok` 后，才记录：

```text
设备已完成配网并建立业务会话
```

仅收到 `wifi_result ok` 时记录：

```text
设备已接受目标 WiFi 配置，等待设备回连
```

## 5. PC 端不承担的职责

- 不负责决定设备状态机何时进入 discovery。
- 不负责保证旧设备热点 TCP 在 STA 切换后继续存在。
- 不负责替设备关闭 SoftAP。
- 不在本版本实现复杂重发协议。
- 不在本版本处理多网卡和复杂路由选择。
- 不在本版本实现真实 mDNS；UDP 组播作为验证主路径。

## 6. PC 端测试

### 正常路径

- 收到 wifi_result ok。
- close_ap 发送成功。
- PC 切回目标 WiFi。
- 收到 device_hello。

### 必须新增的异常路径

- close_ap 发送失败时，PC 准确记录失败并继续切回目标 WiFi。
- 自动切回目标 WiFi 失败时，返回配网结果但提示用户手动切换。
- wifi_result fail 时不发送 close_ap。
- 只有 device_hello/host_ack 完成后才记录业务会话成功。

## 7. PC 端完成标准

- close_ap 日志与真实发送返回值一致。
- close_ap 失败不会导致 PC 一直停留在设备热点。
- PC 能切回稳定目标 WiFi并继续发送 host_announce。
- PC 能收到设备的真实 TCP 连接。
- hello/ack 和至少 3 轮 ping/pong 成功。
