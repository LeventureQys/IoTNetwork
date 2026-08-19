# Integration Tests（跨端集成测试）

本目录**不含任何端侧生产源码**：只消费两端已构建的可执行产物、公开 CLI、
`share/protocol-contract.json`、`--events-jsonl` 事件文件与 `--scenario` 场景文件，
以双进程黑盒方式完成协议 manifest 一致性、帧/协议互操作与 sim 场景验收。

对应验收项：E1/E2、F1~F6、G1/G2/G4、H1；详见
`Document/Update/beta v1.0.5 - 代码完全拆分/验收文档.md`。

## 目录结构

```text
integration_tests/
├── README.md                    # 本文档
├── CMakeLists.txt               # 可选：CTest 注册（需要先构建两端并 install）
├── contracts/
│   ├── README.md                # SS01 既有，只读
│   └── protocol-contract.golden.json   # 冻结的双端协议契约（只读）
├── scenarios/
│   ├── b_first_provision.json   # 首次配网 -> 会话
│   ├── c_graceful_exit.json     # 优雅退出（host_bye/资源清理）
│   ├── d_reconnect.json         # 断链重连（sock_send_fail 注入/恢复）
│   ├── e_close_ap_fail.json     # close_ap 单次发送失败仍上线
│   ├── f_wifi_result_fail.json  # wifi_result fail：无 close_ap、设备保持可配网
│   ├── g_app_data.json          # 双向 app_data：512/513/UTF-8
│   └── h_frame_probe.json       # 帧边界与协议版本（场景 h 由 runner 内建探针驱动）
├── runners/
│   ├── run_scenario.ps1         # PowerShell 5.1+ runner（主实现）
│   └── run_scenario.py          # Python 3 runner（等价实现）
└── scripts/
    ├── run_self_contained.ps1   # G1/G2：复制两端目录到系统临时目录构建/CTest/启动
    └── run_binary_isolation.ps1 # G4：只复制 artifacts + 本目录的二进制隔离运行
```

## 依赖

- Windows 10/11，PowerShell 5.1+ **或** Python 3（两者等价，任选其一；PowerShell 为推荐主实现）。
- 两端已构建产物：`provision_pc.exe`、`provision_device.exe`，以及两端各自的
  `share/protocol-contract.json`（构建目录或 install artifact 均可）。
- Qt 6 MSVC 运行时：运行两端需要 Qt bin 目录（默认 `D:/Devtools/Qt/6.8.3/msvc2022_64/bin`，
  可用 `-QtBin`/`--qt-bin` 或环境变量 `QT_BIN` 覆盖）。
- 自动启动使用 `QT_QPA_PLATFORM=offscreen`（脚本自动设置）。
- 无需真实 WiFi / 无线网卡；全部场景走两端 sim 后端（loopback TCP + 本机组播）。

## 场景清单

| 场景 | 内容 | 关键断言 |
|---|---|---|
| a | protocol manifest 一致性 | PC/设备 `protocol-contract.json` 与 golden 语义相等（忽略 `generated_from`） |
| b | 首次配网 → 会话 | ap_ready → auth_result(ok) → wifi_result(ok) → 双方 session_online；10 秒内 ping/pong；双方 shutdown_complete、退出码 0 |
| c | 优雅退出 | PC request_stop → host_bye_sent；设备 session_offline；无残留进程；TCP 5935/20000 可重绑；sim-catalog 清空 |
| d | 断链重连 | 设备注入 `sock_send_fail` → fault_applied → 双方 session_offline → 恢复 → 30 秒内再 session_online，reconnect_count 增长；重连后 10 秒内 ping/pong |
| e | close_ap 单次发送失败 | PC 注入 `sock_send_fail {"skip":2,"count":1}` → close_ap_sent 失败一次，wifi_result 仍 ok，设备最终上线，无无限重试 |
| f | wifi_result fail | 下发不可达 SSID → 双方 wifi_result=fail；PC 无 close_ap_sent、无 session_online；设备保持可配网（catalog 仍在） |
| g | 双向 app_data | 512 字节双向 tx/rx sha256 一致；513 字节发送端 rejected/`payload_too_large`/无 tx；UTF-8 多字节双向一致 |
| h | 帧边界与版本（尽力而为） | 拆包/粘包帧解析；1025 超长帧头拒绝且连接可用；0 长度帧×3 触发 malformed_max 关闭；PC 拒绝 proto_ver=2、接受 proto_ver=1 |

失败判定：任一进程异常退出、事件缺失、manifest 不一致、超时、残留进程/端口占用、
catalog 残留 → 场景失败。阻塞判定：无两端可执行产物、Qt 缺失、端口被无关进程占用。

## 用法

### PowerShell runner

```powershell
powershell -ExecutionPolicy Bypass -File runners\run_scenario.ps1 `
    -Scenario all `
    -PcExe out\artifacts\pc\bin\provision_pc.exe `
    -DeviceExe out\artifacts\device\bin\provision_device.exe `
    -PcContract out\artifacts\pc\share\protocol-contract.json `
    -DeviceContract out\artifacts\device\share\protocol-contract.json `
    -RuntimeRoot out\ss07-evidence `
    -TimeoutSeconds 45
```

参数：`-Scenario`（`a,b,c,d,e,f,g,h` 或 `all`，默认 all）、`-PcExe`/`-DeviceExe`（必填）、
`-PcConfig`/`-DeviceConfig`（可选，不传则使用 runner 内置的调优配置模板）、
`-PcContract`/`-DeviceContract`（可选，不传则从可执行文件向上搜索 `share/`）、
`-RuntimeRoot`（证据根目录，默认 `%TEMP%\modutech-integration`）、
`-TimeoutSeconds`（默认 45）、`-QtBin`。

### Python runner

```powershell
python runners\run_scenario.py --scenario all `
    --pc-exe out\artifacts\pc\bin\provision_pc.exe `
    --device-exe out\artifacts\device\bin\provision_device.exe `
    --pc-contract out\artifacts\pc\share\protocol-contract.json `
    --device-contract out\artifacts\device\share\protocol-contract.json `
    --runtime-root out\ss07-evidence
```

### CTest 注册（可选）

```powershell
cmake -S demo\integration_tests -B out\integration-build `
    -DINTEGRATION_PC_EXE=<absolute path>\provision_pc.exe `
    -DINTEGRATION_DEVICE_EXE=<absolute path>\provision_device.exe `
    -DINTEGRATION_PC_CONTRACT=<...>\protocol-contract.json `
    -DINTEGRATION_DEVICE_CONTRACT=<...>\protocol-contract.json `
    -DINTEGRATION_RUNTIME_ROOT=<abs>\ss07-evidence-ctest
ctest --test-dir out\integration-build -C Debug --output-on-failure
```

## 证据

每次运行在 `RuntimeRoot` 下按场景创建唯一目录：

```text
<runtime-root>/<timestamp>_<scenario>/
├── pc-events.jsonl / device-events.jsonl   # 双方事件流（JSONL）
├── pc-stdout.log / pc-stderr.log / device-stdout.log / device-stderr.log
├── logs/                                   # 双方诊断日志（log-dir）
├── sim-catalog/                            # 共享模拟 AP catalog 目录
├── scenario.json                           # 拷贝的场景文件
├── configs/                                # runner 生成的配置（或传入的配置）
├── protocol-diff.json                      # 场景 a 的 manifest 差异
└── summary.json                            # 断言清单与通过状态
```

## 自包含验收（G1/G2/G4）

```powershell
# G1/G2：复制 demo/pc、demo/device 到系统临时目录，配置/构建/CTest/offscreen 启动，
#         并扫描构建日志与 compile_commands 不得出现原仓库路径
powershell -ExecutionPolicy Bypass -File scripts\run_self_contained.ps1 `
    -RepoRoot ..\..\.. -QtPrefix D:/Devtools/Qt/6.8.3/msvc2022_64 -OutDir out\ss07-selfcontained

# G4：只复制两端 install artifacts + integration_tests，在无源码目录执行 E/F 场景
powershell -ExecutionPolicy Bypass -File scripts\run_binary_isolation.ps1 `
    -PcArtifact out\artifacts\pc -DeviceArtifact out\artifacts\device `
    -IntegrationRoot demo\integration_tests -QtBin D:/Devtools/Qt/6.8.3/msvc2022_64/bin `
    -RuntimeRoot out\ss07-isolation
```

## 边界

- 本目录禁止 `#include` 端侧私有头、禁止 `add_subdirectory(../pc|../device)`、
  禁止编译/链接端侧任何 `.c/.cpp`/静态库。
- 本目录不得修改两端任何文件；如发现两端行为缺陷（事件缺失、action 名不一致、
  契约漂移），记录到测试报告并回派原 SubStage，不由本目录“适配”。
- `contracts/` 下的 golden 与 README 为 SS01 交付物，只读不改。
