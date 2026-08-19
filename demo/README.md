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
| `pc/` | `demo/pc/README.md` | `provision_pc` | `pc_unit_tests`（103 用例） |
| `device/` | `demo/device/README.md` | `provision_device`（Qt 模拟器，sim/Linux/ESP32 后端） | 设备 CTest（117 用例，含 UI） |
| `integration_tests/` | `demo/integration_tests/README.md` | 双进程 sim 场景 a–h | `run_scenario.ps1` / `run_scenario.py` |

## 双端联调（sim 后端，Windows）

```powershell
# 终端 1：PC（sim 后端）
$env:QT_QPA_PLATFORM="offscreen"
out\pc-artifact\bin\provision_pc.exe --backend sim `
  --config demo\pc\config\pc_config.json --sim-catalog-dir out\cat --duration 60

# 终端 2：设备（sim 后端，同一 sim-catalog-dir）
out\device-artifact\bin\provision_device.exe --backend sim `
  --config demo\device\config\device_sim.json --device-index 0 --fresh `
  --sim-catalog-dir out\cat --duration 60
```

集成测试脚本会自动完成上述双进程编排，见 `integration_tests/README.md`。

## 平台后端说明

- PC：Windows 支持 `windows`（Native WiFi）与 `sim` 后端；Linux 仅支持 `sim`。
- 设备：Windows/Linux 支持 `sim`；Linux 支持真实热点/STA（`linux`，fail-closed，失败不回退 sim）；`firmware/esp32c2/` 为 ESP-IDF 移植骨架，实机功能未在本仓库验证。

## 文档

- 协议契约：`integration_tests/contracts/protocol-contract.golden.json`（PC 与设备各自生成 `share/protocol-contract.json`，集成测试强制语义一致）。
- 版本开发过程文档：仓库根 `Document/Update/beta v1.0.5 - 代码完全拆分/`。
