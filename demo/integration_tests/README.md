# Integration Tests（跨端集成测试）

本目录**不含任何端侧生产源码**：只消费两端已构建的可执行产物、公开 CLI、
`share/protocol-contract.json`、`--events-jsonl` 事件文件与 `--scenario` 场景文件，
以双进程黑盒方式完成协议 manifest 一致性、帧/协议互操作与 sim 场景验收。

对应 beta v1.1 验收：设计文档第 15 节（sim 双进程场景 A~H）与启动测试第 6 节。
历史对应关系见 `Document/Update/beta v1.1/验收文档.md`。

## 目录结构

```text
integration_tests/
├── README.md                    # 本文档
├── CMakeLists.txt               # 可选：CTest 注册（需要先构建两端并 install）
├── contracts/
│   ├── README.md                # SS01 既有，只读
│   └── protocol-contract.golden.json   # 冻结的双端协议契约（只读，schema 2）
├── scenarios/
│   ├── b_hotspot_session.json   # B：热点→设备直连→会话→心跳
│   ├── c_pc_stop.json           # C：PC 优雅退出→catalog 删除→设备离线
│   ├── d_reconnect.json         # D：sock_send_fail 注入→恢复→固定地址重连
│   ├── e_second_device.json     # E：第二设备被拒（single_device_only）
│   ├── f_no_target.json         # F：目标 SSID 不存在→持续扫描、无会话
│   ├── g_app_data.json          # G：双向 app_data 512/513/UTF-8
│   └── h_frame_probe.json       # H：帧边界与版本（runner 内建 socket 探针）
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
- 无需真实 WiFi / 无线网卡；全部场景走两端 sim 后端（PC 发布 `pc-hotspot.json`
  catalog，设备扫描/连接并翻译固定地址 `192.168.137.1:5935` 到 loopback）。

## 场景清单（beta v1.1，A~H）

| 场景 | 内容 | 关键断言 |
|---|---|---|
| a | 三份 schema2 contract 语义一致 | PC/设备 `protocol-contract.json` 与 golden 语义相等（忽略 `generated_from`），schema_version=2 |
| b | 热点→设备直连→会话 | PC `hotspot_ready→tcp_listening→ready`；设备 `ready→wifi_scan_started→wifi_target_found→wifi_connected→tcp_connecting→session_online`；10 秒内完整 ping/pong 回环；双方 shutdown_complete、退出码 0 |
| c | PC 优雅退出 | PC `request_stop` → `pc-hotspot.json` 删除 → 设备 `session_offline`；无残留进程/端口/catalog；设备事件流不含任何配网/热点事件 |
| d | TCP 故障重连 | 设备注入 `sock_send_fail` → 双方 `session_offline` → 恢复 → 30 秒内固定地址重连 `session_online`，reconnect_count 增长；重连后 ping/pong |
| e | 第二设备被拒 | 设备1在线时设备2收到 `host_ack busy/single_device_only`（PC 事件 `single_device_rejected`），设备2 无 `session_online`，设备1保持在线 |
| f | 目标不存在 | 设备配置 `pc_ap_ssid=Modu_Other`（与 PC 发布不一致）→ 无 `session_online`、无任何配网/热点事件、设备持续 `wifi_scan_started` |
| g | 双向 app_data | 512 字节双向 tx/rx sha256 一致；513 字节发送端 `rejected/payload_too_large`/无 tx；UTF-8 多字节双向一致 |
| h | 帧边界与版本（PC-only） | 拆包帧完整解析；1025 超长帧头拒绝且连接可用；0 长度帧×3 触发 malformed_max 关闭；`proto_ver=2` 被拒（host_ack fail/unsupported_protocol）；`proto_ver=1` 接受；在线连接粘包 2 帧 ping → 2 个 pong |

失败判定：任一进程异常退出、事件缺失/乱序、manifest 不一致、超时、残留进程/端口占用、
catalog 残留 → 场景失败。阻塞判定：无两端可执行产物、Qt 缺失、端口被无关进程占用。

> 说明 1：设备首个 ping 可能与 `device_hello` 同段到达 PC，PC 在 pending→online
> 转换时丢弃该段内后续帧（端侧行为，良性——心跳 2 秒重发），因此 ping/pong 断言
> 匹配**任意**完整回环对而非首个 ping。
>
> 说明 2：sim 后端在 catalog 删除后 STA 状态仍保持"已连接"，设备按设计文档 9.2
> 进入 HEAL 的 TCP 退避重连（不产生扫描事件）；真实 Linux 后端热点消失时 STA 掉线
> 会回扫描。该差异属 sim 保真度范围，不属端侧缺陷。

## 用法

### PowerShell runner

```powershell
powershell -ExecutionPolicy Bypass -File runners\run_scenario.ps1 `
    -Scenario all `
    -PcExe out\pc-build\bin\Debug\provision_pc.exe `
    -DeviceExe out\device-build\bin\Debug\provision_device.exe `
    -PcContract demo\pc\share\protocol-contract.json `
    -DeviceContract out\device-build\share\protocol-contract.json `
    -RuntimeRoot out\v11-integration-evidence `
    -TimeoutSeconds 60 `
    -QtBin D:/Devtools/Qt/6.8.3/msvc2022_64/bin
```

参数：`-Scenario`（`a,b,c,d,e,f,g,h` 或 `all`，默认 all）、`-PcExe`/`-DeviceExe`（必填）、
`-PcConfig`/`-DeviceConfig`（可选，不传则使用 runner 内置的 v1.1 调优配置模板）、
`-PcContract`/`-DeviceContract`（可选，不传则从可执行文件向上搜索 `share/`）、
`-RuntimeRoot`（证据根目录，默认 `%TEMP%\modutech-integration`）、
`-TimeoutSeconds`（默认 60）、`-QtBin`、`-Quiet`。

编排顺序（任务书第 5 节）：先启动 PC sim 并等待 `hotspot_ready`，再启动设备；
两端共享同一 `--sim-catalog-dir`。场景 e 为 PC→设备1（等待 `session_online`）→设备2
三进程编排；场景 h 仅启动 PC（唯一连接槽留给原始 socket 探针）。

### Python runner

```powershell
python runners\run_scenario.py --scenario all `
    --pc-exe out\pc-build\bin\Debug\provision_pc.exe `
    --device-exe out\device-build\bin\Debug\provision_device.exe `
    --pc-contract demo\pc\share\protocol-contract.json `
    --device-contract out\device-build\share\protocol-contract.json `
    --runtime-root out\v11-integration-evidence
```

### CTest 注册（可选）

```powershell
cmake -S demo\integration_tests -B out\integration-build `
    -DINTEGRATION_PC_EXE=<absolute path>\provision_pc.exe `
    -DINTEGRATION_DEVICE_EXE=<absolute path>\provision_device.exe `
    -DINTEGRATION_PC_CONTRACT=<...>\protocol-contract.json `
    -DINTEGRATION_DEVICE_CONTRACT=<...>\protocol-contract.json `
    -DINTEGRATION_RUNTIME_ROOT=<abs>\v11-integration-ctest
ctest --test-dir out\integration-build -C Debug --output-on-failure
```

## 证据

每次运行在 `RuntimeRoot` 下按场景创建唯一目录：

```text
<runtime-root>/<timestamp>_<scenario>/
├── pc-events.jsonl / device-events.jsonl  # 双方事件流（JSONL；场景 e 另有 dev1/dev2）
├── pc-stdout.log / pc-stderr.log / device-stdout.log / device-stderr.log
├── logs/                                   # 双方诊断日志（log-dir）
├── sim-catalog/                            # 共享模拟 AP catalog 目录（pc-hotspot.json）
├── scenario.json                           # 拷贝的场景文件
├── configs/                                # runner 生成的 v1.1 配置（或传入的配置）
├── protocol-diff.json                      # 场景 a 的 manifest 差异
└── summary.json                            # 断言清单（每断言 name/status/evidence）与通过状态
```

`summary.json` 结构：`{scenario, pass, timestamp, assertions:[{name,status,evidence}], note}`。

## 自包含验收（G1/G2/G4）

```powershell
# G1/G2：复制 demo/pc、demo/device 到系统临时目录，配置/构建/CTest/offscreen 启动，
#         并扫描构建日志与 compile_commands 不得出现原仓库路径
powershell -ExecutionPolicy Bypass -File scripts\run_self_contained.ps1 `
    -RepoRoot ..\..\.. -QtPrefix D:/Devtools/Qt/6.8.3/msvc2022_64 -OutDir out\v11-selfcontained

# G4：只复制两端 install artifacts + integration_tests，在无源码目录执行 A~H 场景
powershell -ExecutionPolicy Bypass -File scripts\run_binary_isolation.ps1 `
    -PcArtifact out\pc-artifact -DeviceArtifact out\device-artifact `
    -IntegrationRoot demo\integration_tests -QtBin D:/Devtools/Qt/6.8.3/msvc2022_64/bin `
    -RuntimeRoot out\v11-isolation
```

## 边界

- 本目录禁止 `#include` 端侧私有头、禁止 `add_subdirectory(../pc|../device)`、
  禁止编译/链接端侧任何 `.c/.cpp`/静态库。
- 本目录不得修改两端任何文件；如发现两端行为缺陷（事件缺失、action 名不一致、
  契约漂移），记录到测试报告并回派原 SubStage，不由本目录"适配"。
- `contracts/` 下的 golden 与 README 为 SS01 交付物，只读不改。
