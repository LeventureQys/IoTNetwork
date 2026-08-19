# ModuTech Demo 工程

`demo/` 已拆分为两个完全自包含的产品目录 + 一个跨端集成测试目录，三者互不引用生产源码：

```text
demo/
├── pc/                   # PC 上位机（自包含，C++17 + Qt）
├── device/               # 设备端（自包含：Qt 模拟器外壳 + 纯 C 核心/后端 + ESP32 移植骨架）
└── integration_tests/    # 跨端集成测试（只消费两端构建产物，不含任何生产源码）
```

每个产品目录均可单独复制到仓库外构建、测试与运行，不依赖本目录其他内容。

## 快速导航

| 目录 | 构建 | 运行 | 测试 |
|------|------|------|------|
| `pc/` | `demo/pc/README.md` | `provision_pc` | `pc_unit_tests` / `pc_ui_tests` / `pc_contract_tests` |
| `device/` | `demo/device/README.md` | `provision_device`（Qt 模拟器，sim/Linux/ESP32 后端） | 设备 CTest（runtime/config/core/sim/linux/esp32/ui） |
| `integration_tests/` | `demo/integration_tests/README.md` | 双进程 sim 场景 a–h | `run_scenario.ps1` / `run_scenario.py` |

## 拓扑（beta v1.1）

一对一固定拓扑，无配网、无服务发现、无设备热点：

```text
Windows PC（Windows 10/11）
  1. 创建 SSID=Modu_PC（可配置，须 Modu_ 前缀）的 WPA2 移动热点
  2. 校验/配置热点 IPv4 = 192.168.137.1/24
  3. 监听 0.0.0.0:5935，等待唯一 Linux 设备连接

Linux Device
  1. 扫描 WiFi，查找精确 SSID（pc_ap_ssid，默认 Modu_PC）
  2. 用固定密码 modu_leventure 连接，获取 DHCP IPv4
  3. 直连 192.168.137.1:5935，device_hello → host_ack
  4. ping/pong 心跳 + app_data；WiFi/TCP 失败退避重试
```

sim 后端用 `pc-hotspot.json` catalog（schema 2）在双进程间模拟该拓扑，两端共享同一
`--sim-catalog-dir` 即可联调，无需真实网卡。

## 双端联调（sim 后端，Windows）

```powershell
# 终端 1：PC（sim 后端）—— 先启动并等待其发布 catalog
$env:QT_QPA_PLATFORM="offscreen"
out\pc-build\bin\Debug\provision_pc.exe --backend sim `
  --config demo\pc\config\pc_config.json --sim-catalog-dir out\cat `
  --events-jsonl out\pc-events.jsonl --duration 60

# 终端 2：设备（sim 后端，同一 sim-catalog-dir）
out\device-build\bin\Debug\provision_device.exe --backend sim `
  --config demo\device\config\device_sim.json --device-index 0 --fresh `
  --sim-catalog-dir out\cat --events-jsonl out\dev-events.jsonl --duration 60
```

集成测试脚本会自动完成上述双进程编排（先等 `hotspot_ready` 再启动设备），
见 `integration_tests/README.md`。

## 平台后端说明

- PC：Windows 支持 `windows`（现代移动热点 WinRT，需支持移动热点的无线网卡）与 `sim` 后端；
  Linux 仅支持 `sim`。
- 设备：Windows/Linux 支持 `sim`；Linux 支持真实 STA（`--backend linux`，仅需 nmcli/网卡权限，
  纯 STA 不要求 root，失败 fail-closed 不回退 sim）；`firmware/esp32c2/` 为 ESP-IDF 移植骨架，
  **beta v1.1 明确不纳入开发与验收范围**（无设备热点/配网业务，ESP32 不适用本拓扑）。

## 文档

- 协议契约：`integration_tests/contracts/protocol-contract.golden.json`（PC 与设备各自生成
  `share/protocol-contract.json`，集成测试强制语义一致）。
- 版本开发过程文档：仓库根 `Document/Update/beta v1.1/`。
