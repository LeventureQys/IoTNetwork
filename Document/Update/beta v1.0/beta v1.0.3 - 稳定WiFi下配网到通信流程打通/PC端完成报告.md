# beta v1.0.3 PC 端完成报告（阶段三）

## 1. 完成范围

按 `PC端修改计划.md` 与 `PC端流程差异.md` 完成 Stage 2（PC 端配网收尾与结果日志），未触碰设备状态机源码与协议命令定义。

### 1.1 close_ap 发送结果检查（P1）

- `demo/pc/core/host_provision_client.cpp`：
  - `send_frame()` 修改为完整写判定：
    - `frame_wrap` 失败：原错误码原样返回；
    - 底层负错误码：原样返回；
    - 返回非负但小于帧长（部分写入）：`DEMO_ERR`；
    - 完整写入整帧：`DEMO_OK`。
  - 本版本不实现循环重试（与阶段二 Q4 自决一致）。
  - `ProvisionDevice()`：
    - auth 写入失败：记录 `配网：auth 写入配网连接失败，返回码=%d，本次配网结束`，结束本次配网并返回失败。
    - wifi_config 写入失败：记录 `配网：wifi_config 写入配网连接失败，返回码=%d，本次配网结束`，结束本次配网并返回失败。
    - close_ap 写入成功：`配网：close_ap 已完整写入配网连接`；写入失败：`配网：close_ap 写入失败，返回码=%d，设备将自动完成交接`。两种结果均返回 WiFi 配置成功（`0`），不无限重试、不停留在设备热点。

### 1.2 阶段日志语义（P3）

- 收到 `wifi_result ok` 时记录：`设备已接受目标 WiFi 配置：设备热点=<AP>，目标SSID=<SSID>，设备IP=<IP>，等待设备回连`。
- `demo/pc/core/host_tcp_server.cpp`：收到 `device_hello` 并发送 `host_ack ok` 后记录：`设备已完成配网并建立业务会话：<id>`。两类成功日志严格分离。

### 1.3 切网日志（P2）

- 结束配网连接顺序固定：关闭配网 TCP → 记录 `正在断开设备热点` → `net_wifi_sta_disconnect()` → 真实 WiFi 模式记录 `正在切回目标 WiFi <SSID>` → `net_wifi_sta_connect()` → 成功记录 `已切回目标 WiFi <SSID>`；失败记录 `未能自动切回目标 WiFi <SSID>，原因码=<rc>，请手动连接`。

### 1.4 最小故障注入（测试支撑）

- `demo/net_sim/sim_backend.cpp`：新增注入动作 `sock_send_fail`，参数 `{"skip":N,"count":M}`：前 N 次发送正常，随后连续 M 次发送强制失败（`DEMO_ERR`）。仅作用于本后端实例，不改变正常路径行为。

### 1.5 测试

- `demo/tests/test_host_app.cpp`：
  - `E2EFixture::Start()` 增加 `auto_provision` 参数（默认 true，既有用例不变）。
  - 新增 `HostE2E.CloseApSendFailureStillRegisters`：注入 close_ap 发送失败后，断言配网在限定时间内结束（无死循环）、`wifi_result ok` 收到、close_ap 失败被准确记录且仅一次（无重试）、配网结果仍为成功。
  - 新增 `HostE2E.WifiResultFailSkipsCloseAp`：目标 WiFi 认证失败注入后，断言 PC 不发送 close_ap、返回配网失败、日志保留具体 reason。

### 1.6 文档同步

- `demo/README.md`：补充 close_ap 最佳努力语义与"等待回连/业务会话建立"两级成功描述。
- `demo/协议文档.md` 5.1.3：补充最佳努力语义（发送一次、失败仅记录、设备宽限期自动交接、不视为错误）。

## 2. 修改文件列表

| 文件 | 修改内容 |
|---|---|
| `demo/pc/core/host_provision_client.cpp` | send_frame 完整写判定；auth/wifi_config/close_ap 发送结果检查；阶段与切网日志 |
| `demo/pc/core/host_tcp_server.cpp` | host_ack ok 后新增"设备已完成配网并建立业务会话"日志 |
| `demo/net_sim/sim_backend.cpp` | 新增 `sock_send_fail` 最小故障注入 |
| `demo/tests/test_host_app.cpp` | fixture 支持延迟配网；新增 2 个测试 |
| `demo/tests/CMakeLists.txt` | 修复 v1.0.3 提交（489ab88）引入的 Windows 构建回归：`test_linux_hotspot_config.cpp` 移回 Linux 条件块（v1.0.2 既有决策，该提交误改回退；恢复为 e380665 状态） |
| `demo/README.md`、`demo/协议文档.md` | close_ap 最佳努力语义同步 |

未新增协议命令、未修改设备状态机、未修改 UDP host_announce JSON、未实现 `close_ap_ack`/重试/等待 device_hello。

## 3. close_ap 日志样例

成功路径（回归 `HostE2E.ProvisionRegisterHeartbeat`）：

```text
设备已接受目标 WiFi 配置：设备热点=Modu_0001，目标SSID=<SSID>，设备IP=<IP>，等待设备回连
配网：close_ap 已完整写入配网连接
正在断开设备热点
配网结果：Modu_0001 成功
```

失败路径（注入 `sock_send_fail {"skip":2,"count":1}`，实测输出）：

```text
设备已接受目标 WiFi 配置：设备热点=Modu_0001，目标SSID=TactileFactory-2.4G，设备IP=192.168.1.100，等待设备回连
故障注入：SockSend 被强制失败（剩余 0 次）
配网：close_ap 写入失败，返回码=-1，设备将自动完成交接
正在断开设备热点
配网结果：Modu_0001 成功
```

## 4. 测试名称与结果

本机（VS2022 MSVC 17 / Windows 10 / Debug，/W4 /WX）构建退出 0，无本版本新增警告。

| 测试 | 结果 |
|---|---|
| `HostE2E.ProvisionRegisterHeartbeat`（正常 E2E 回归） | 通过 |
| `HostE2E.CloseApSendFailureStillRegisters`（新增） | 通过（294ms） |
| `HostE2E.WifiResultFailSkipsCloseAp`（新增） | 通过（280ms） |
| `provision_demo_tests` 全量 | **88/88 通过** |
| `test_device_ap_backoff` | 2/2 通过 |

## 5. 模拟 E2E 结果

- 正常路径（auth → wifi_config → wifi_result ok → close_ap 成功 → 切回目标 WiFi → device_hello/host_ack → 心跳）：`HostE2E.ProvisionRegisterHeartbeat` 通过，无回归。
- close_ap 失败路径：`HostE2E.CloseApSendFailureStillRegisters` 通过，无死循环、无无限重试，WiFi 配置结果未被错误改为失败。
- wifi_result fail 路径：`HostE2E.WifiResultFailSkipsCloseAp` 通过，PC 不发送 close_ap、返回失败、保留 reason。

> "设备最终上线"（修改计划 5.2 目标 6）依赖设备端自动交接（Stage 1），由设备端测试与集成验收 E-A3 覆盖，PC 侧测试不承担。

## 6. 构建结果

```powershell
cmake --build tests/build-msvc --config Debug -j8
```

全部目标（demo_common / demo_sim_backend / device_core / pc_core / provision_demo_tests / test_device_ap_backoff）构建成功。

## 7. 待真实环境验收项

| 项 | 内容 | 原因 |
|---|---|---|
| P-A2（真实） | 真实 close_ap 失败不阻塞切网 | 需真实设备热点；模拟注入已覆盖 |
| P-A4 | 切回目标 WiFi 后实际 SSID 校验 | 需真实 WiFi，无法在本机自动执行 |
| E-A2 / E-A3 | 真实稳定 WiFi 主流程（host_announce → device_hello → host_ack → ≥3 轮 ping/pong） | 依赖设备端 Stage 1 完成 + 同一局域网真实验收 |

真实联调时提供的连续 PC 日志顺序应为：`设备已接受目标 WiFi 配置…等待设备回连` → `配网：close_ap 已完整写入配网连接`（或写入失败日志）→ `正在断开设备热点` → `正在切回目标 WiFi <SSID>` → `已切回目标 WiFi <SSID>` → UDP host_announce → `设备已完成配网并建立业务会话：<id>` → ping/pong。

## 8. 完成标准对照（PC端流程差异 第 7 节）

- close_ap 日志与真实发送返回值一致：✅ 成功/失败两种日志由测试断言。
- close_ap 失败不导致 PC 停留在设备热点：✅ 测试断言流程按时结束、配网结果成功。
- PC 能切回稳定目标 WiFi 并继续发送 host_announce：✅ 代码路径；真实联调待执行。
- 收到设备真实 TCP 连接、hello/ack、≥3 轮 ping/pong：待真实环境（E-A2）。

## 9. 已知问题与说明

1. `demo/tests/CMakeLists.txt`：提交 489ab88（v1.0.3 文档提交）把 `test_linux_hotspot_config.cpp`（依赖 net_linux 头文件）误放回无条件编译列表，导致 Windows 测试工程无法编译；已按 v1.0.2 既定决策恢复为 Linux 条件块，Linux 行为不变。该修复属于 v1.0.3 行内回归修复，非范围外改动。
2. 本版本不检查 `HostTcpServer::SendFrame` 的写结果（沿用既有 `rc >= 0` 判定），属阶段二 Q4 明确的范围边界：公共 TCP 发送重构留待后续版本。
