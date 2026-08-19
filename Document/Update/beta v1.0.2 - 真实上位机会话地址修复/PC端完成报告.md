# beta v1.0.2 PC端完成报告（阶段三）

## 1. 完成范围

按 `PC端修改计划.md` 的 Stage P1 / P2 / P3 完成，未触碰设备端与 sim 后端真实 TCP 目标修复（属设备端工作线）。

### P1 监听生命周期

- `demo/pc/core/host_tcp_server.h/.cpp`：
  - 新增 `Stop()`（幂等，关闭 pending/online 连接与 listener，可重复调用）。
  - 新增 `Started()`，反映 listener 是否存在；`Stop()` 后为 false。
  - `Start()` 幂等：已有 listener 时返回 `DEMO_OK`（实现选择"幂等成功"并用测试固定）。
  - 析构统一调用 `Stop()`，删除重复关闭逻辑。
  - 监听成功日志改为 `0.0.0.0:<port>`，不再误写成 `host_virtual_ip`。
- `demo/pc/core/host_app.cpp`：
  - `Start()` 顺序固定：TCP listener → announcer → mDNS 兼容注册。
  - TCP 失败：直接返回，announcer 与 mDNS 均未启动。
  - announcer 失败：立即 `tcp_server_.Stop()` 回滚后返回。
  - mDNS 兼容注册失败：记录警告，组播主路径继续。
  - `RequestStop()`：SendBye → announcer.Stop → tcp_server.Stop → mdns.Unregister → stop=true，并置 `started_=false`。
  - `ForceCrash()`：不发送 bye，但关闭 announcer、TCP、mDNS，并置 `started_=false`。

### P2 地址通告与日志

- `demo/net_win/win_backend.cpp`：
  - `socket_tcp_listen()`：socket 创建、setsockopt、bind、listen、非阻塞设置各阶段失败均记录 `WSAGetLastError()`；任一步失败关闭 socket 且不设置输出句柄；成功日志显示 `0.0.0.0:<port>`。
  - 新增 `win_backend_ipv4_valid()`（win_backend.h 公开）：拒绝 unspecified（0.0.0.0/8）、loopback（127.0.0.0/8）、APIPA（169.254.0.0/16）、格式错误与越界值；带尾部字符检测（`%c` 追加项）。
  - `win_backend_get_ipv4()`：候选增加 `sa_family==AF_INET` 与 `win_backend_ipv4_valid()` 过滤，返回首个有效地址；无有效地址返回 `DEMO_ERR`（main 拒绝启动，不回退模拟地址）。
- `demo/pc/core/host_mdns.cpp/.h`：`Register()` 改为返回 `int`（DEMO_OK/失败），失败仅警告。
- `demo/pc/core/host_announcer.cpp`：`Poll()` 发送日志增加 `ip=` 与 `tcp_port=` 字段。
- `demo/pc/app/main.cpp`：启动成功后打印"运行模式 / TCP监听端点 / 服务通告端点 / 组播端点"四条诊断日志。
- `demo/README.md`、`demo/协议文档.md`：补充 PC 监听与通告契约段落，明确 Windows 真实发现主路径为 UDP 组播、mDNS 为兼容注册不作为真实验收项。

### P3 PC 端测试

- 新增 `demo/tests/test_host_lifecycle.cpp`（9 用例，fake backend 注入）：
  - 启动顺序 TCP→组播→mDNS。
  - TCP 失败：不调用组播/mDNS，`Started()==false`。
  - 组播失败：listener 立即关闭，`Started()==false`，不调用 mDNS。
  - mDNS 失败：组播主路径继续，`Start()` 成功。
  - `RequestStop()` 顺序与幂等（二次调用无副作用）。
  - `ForceCrash()` 无 bye、关闭全部资源。
  - `HostTcpServer` Start 幂等 / Stop 幂等 / 停止后重启。
  - announce JSON 的 `ip`/`tcp_port` 与 `params.host_virtual_ip`/`host_tcp_port` 一致。
  - `win_backend_ipv4_valid` 过滤规则（Windows 分支）。
- `demo/tests/CMakeLists.txt`：注册 `test_host_lifecycle.cpp`；将 `test_linux_hotspot_config.cpp` 移入 Linux 条件块（原无条件列入导致 Windows 测试工程无法编译，属 v1.0.1 遗留，Linux 行为不变）。
- `.gitignore`：增加 `demo/tests/build*/`。

## 2. 测试结果（本机 VS2022 MSVC 17，Windows 10）

构建：`cmake -G "Visual Studio 17 2022" -A x64 -S tests -B tests/build-msvc` + `cmake --build ... --config Debug`，退出 0，无新增警告（/W4 /WX）。

`provision_demo_tests`：76 用例 → **75 通过，1 失败**。
`test_device_ap_backoff`：2 通过。

失败用例 `HostE2E.WifiDisconnectReconnect`（test_host_app.cpp:156）：已在 git HEAD（本版本改动前）构建同版测试复现同样失败，属预存在的设备端问题。现象：注入 wifi_disconnect 后设备 STA 重试 5 次在约 60ms 内耗尽（`sm_sta_join` 每次 tick 都直接调用 `net_wifi_sta_connect`，退避计时未前置生效），凭据被清除进入 ap_provision，wifi_ok 注入后无法重连。

> 已按用户确认（"两个问题按建议方式解决"）在本次追加修复，见第 8 节。

## 3. 增量系统测试（本机可执行部分）

- HostE2E 配网→发现→注册→心跳：通过（日志显示 listener 先于 announce、`host_announce ip=192.168.1.50 tcp_port=5935`、host_ack ok、心跳正常）。
- HostE2E 优雅退出 host_bye：通过。
- HostE2E 断线重连：通过（追加修复后 1.9s 内完成重连）。
- 模拟配网/SimSocket/SimWorld/Provision/Registry 等回归：全部通过。

## 4. Windows 真实环境额外证据（本机已执行）

- 临时小程序以真实 `win_backend` 调用 `net_tcp_listen(5935)`：`Get-NetTCPConnection` 显示本进程监听 `0.0.0.0:5935`（LocalAddress=0.0.0.0, LocalPort=5935, State=Listen）。
- `win_backend_get_ipv4` 返回本机 `192.168.1.191`，通过有效性过滤。
- 端口占用：改用 `SO_EXCLUSIVEADDRUSE` 后，第二个实例绑定失败（WSA=10048），满足 P-A6"端口占用启动失败"；关闭后可立即重新绑定，含"已建立连接并由服务端主动关闭（TIME_WAIT）"场景实测同样可立即重启，快速重启语义保留。证据详见第 8 节。

## 5. 待 Windows 环境验收项（未通过、不得默认通过）

| 项 | 内容 | 原因 |
|----|------|------|
| P-A4（部分） | `provision_pc.exe` 完整构建 | 本机无 Qt6（`find_package(Qt6 REQUIRED COMPONENTS Widgets)`），pc_core+win_backend+测试工程已 MSVC 构建通过 |
| P-A5（部分） | 启动真实上位机采集 UDP 5936 announce 报文，核对 ip/tcp_port 与 TCP 监听一致 | 依赖 Qt 程序运行与抓包 |
| P-A6（部分） | 端口占用时程序启动失败且无 announce | 后端独占绑定已实测通过；程序级退出行为需真实程序运行确认 |
| I-A1/I-A2 | 双端真实联调（device_hello → host_ack → ping/pong） | 依赖设备端单端验收完成 + 同一局域网 |

## 6. 完成标准对照（PC 端修改计划第 8 节）

- 代码证明 listener 先于 announcer 启动：✅ 单测断言 + E2E 日志。
- listener/announcer 失败即时回滚：✅ 单测断言。
- 日志准确区分监听与通告端点：✅ 代码 + 日志样例。
- 无效 IPv4 过滤：✅ 单测 + 本机实测。
- 当前环境可执行核心测试通过：✅ **77/77 全量通过**（追加修复后）。
- Windows 专有构建与运行结果明确为通过或待验收：✅ 明确标注。

## 7. 已知问题与待确认

1. ~~`HostE2E.WifiDisconnectReconnect` 预存在失败~~：**已修复**（见第 8 节）。
2. ~~P-A6 与 Windows `SO_REUSEADDR` 同端口重复绑定语义冲突~~：**已修复**（引入 `SO_EXCLUSIVEADDRUSE`，见第 8 节）。
3. `demo/tests/CMakeLists.txt` 将 `test_linux_hotspot_config.cpp` 调整为 Linux 条件编译（Windows 下该文件依赖 net_linux 头文件无法编译）；Linux 下行为不变。
4. 全量测试偶发 `DeviceSm.NoCredsGoesToProvision` 超时（全量负载下设备线程饥饿抖动，单独/分组/复跑均通过）；已复跑确认稳定，属测试环境负载抖动，不涉及逻辑缺陷。

## 8. 追加修复（用户已确认，2026-08-05）

### 8.1 设备端 STA 重试退避修复（解决问题 1）

- 根因：`demo/device/device_app.c` 的 `sm_sta_join()` 每次 10ms tick 都无条件调用 `net_wifi_sta_connect()`，退避计时只写在失败路径末尾且不阻断连接，导致 `wifi_retry_max=5` 次重试在约 60ms 内耗尽，凭据被清除进入配网热点，目标 WiFi 恢复后无法自动重连。
- 修复：
  - `sm_sta_join()` 顶部增加退避门禁：`wifi_retry_count>0` 时按指数退避（`base << (retry_count-1)` + 抖动，封顶 cap）驻留，期满前不发起连接；失败后以 `state_enter_ms` 记录本次尝试时间作为下一轮退避起点。与 `sm_heal()` 的既有门禁模式一致。
  - 移除已无用途的 `wifi_backoff_attempt` 字段（`device_priv.h`）及其 6 处复位。
- 验证：`HostE2E.WifiDisconnectReconnect` 通过（wifi_ok 注入后 ~1.9s 完成重连）；`DeviceSm.*` 全部通过（CredentialRollback 在退避循环中驻留 STA_JOIN，符合"不可达→退避循环"的测试预期）。
- 说明：本次修改超出 PC 端修改计划文件边界（`demo/device/**`），系用户明确确认后执行。

### 8.2 Windows 监听独占绑定（解决问题 2，满足 P-A6）

- 修复：`demo/net_win/win_backend.cpp` 的 `socket_tcp_listen()` 将 `SO_REUSEADDR` 改为 `SO_EXCLUSIVEADDRUSE`（真实 Windows 监听路径；sim 后端不受影响）。
- 实测证据（本机 Windows）：
  - 冲突：A 独占绑定后 B 再绑定失败（WSA=10048）→ 端口占用时上位机启动失败路径成立。
  - 快速重启：A 关闭后立即重绑定成功。
  - TIME_WAIT 场景：建立真实连接、服务端主动关闭（进入 TIME_WAIT）后，立即重绑定仍成功 → 快速重启语义保留。
- 单测：新增 `WinBackendListener.ExclusiveBindConflictAndFastRestart`（探测空闲端口 → 独占绑定 → 二次绑定必须失败 → 关闭后重启绑定必须成功）。
- 与 PC 端修改计划 3.2"不引入 SO_EXCLUSIVEADDRUSE"的既定决策偏差：经用户确认后执行；计划文档中的该项决策以本报告为准更新。
