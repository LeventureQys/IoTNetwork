# demo/device — 设备端（自包含）

TactileSense 下位机设备模拟器。本目录完全自包含：构建、源码、配置、第三方、
测试、脚本与协议产物均不引用仓库根级或 PC 端任何目录。单独复制本目录到任意
位置后即可配置、构建、CTest 与 offscreen 启动。

## 目录布局

| 路径 | 内容 |
|------|------|
| `app/qt_main.cpp` | 最薄 Qt 启动入口（唯一生产 C++ 入口之一） |
| `ui/` | 设备 Qt 外壳：`device_window`（facade-only）+ `log_model` + `flow_widget` |
| `include/` | 纯 C 公开契约头（facade/protocol/net 抽象/配置/后端工厂） |
| `src/common/` | 设备公共 C：frame/log/net_abstraction/protocol |
| `src/config/` | CLI 解析（host_options）、配置加载、路径解析 |
| `src/runtime/` | 生命周期/线程/命令与日志队列/事件写入器/scenario 引擎/facade 实现 |
| `src/core/` | 设备业务状态机（boot→sta→ap→discovery→connect→session→heal） |
| `src/platform/{win32,posix}/` | 纯 C 平台适配（线程/锁/单调时钟） |
| `backends/sim/` | 纯 C 模拟后端（WiFi/AP/TCP/NVS/catalog/fault） |
| `backends/linux/` | Linux 真实热点/STA 后端（非 Linux 平台为 NOT_SUPPORTED stub） |
| `backends/esp32c2/` | ESP32-C2 纯 C 移植骨架 |
| `firmware/esp32c2/` | ESP-IDF 工程入口（不含宿主 Qt 程序） |
| `config/` | `device_sim.json` / `device_linux.json` / `linux_hotspot.json` |
| `scripts/` | `start_device.sh` / `start_device.ps1` |
| `tests/` | gtest/CTest 聚合（runtime/config/core/sim/linux/esp32/ui） |
| `third_party/` | cjson、googletest-1.15.2（私有副本） |
| `cmake/` | 协议契约模板（生成 `share/protocol-contract.json`） |

## 语言边界

生产 C++ 仅允许 `app/qt_main.cpp` 与 `ui/`（C++17/Qt/AUTOMOC 只作用于
`device_ui` 与 `provision_device`）。`src/`、`backends/`、`firmware/` 为纯 C11，
禁止 `.cpp`、Qt include、`std::`、`new/delete`、C++ class；core/runtime/backend
构建图不链接 Qt。

## 构建（Windows，MSVC 17 2022 + Qt 6.8.3）

```powershell
cmake -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="D:/Devtools/Qt/6.8.3/msvc2022_64" `
  -DDEVICE_BUILD_TESTS=ON -S demo/device -B out/device
cmake --build out/device --config Debug --parallel 8
ctest --test-dir out/device -C Debug --output-on-failure
```

选项：`DEVICE_BUILD_TESTS`（ON 时 `add_subdirectory(tests)`）、
`DEVICE_WARNINGS_AS_ERRORS`（设备生产代码告警即错；第三方不继承）。

## 运行（sim 后端，offscreen）

```powershell
$env:QT_QPA_PLATFORM="offscreen"
out/device/bin/Debug/provision_device.exe `
  --config demo/device/config/device_sim.json `
  --backend sim --device-index 0 --fresh `
  --runtime-dir out/device-run --sim-catalog-dir out/device-cat `
  --events-jsonl out/device-events.jsonl --duration 3
```

CLI：`--config`、`--device-index`(0~15)、`--fresh`、`--duration`、
`--backend sim|linux`（Linux 后端非 Linux 平台返回错误，不回退 sim）、
`--runtime-dir`、`--sim-catalog-dir`、`--log-dir`、`--events-jsonl`、`--scenario`。
不传测试参数时无测试通道。相对路径按启动 CWD 解析；配置内相对路径按配置文件目录解析。

## Scenario（`--scenario`，仅 sim 后端）

文件 schema 见设计文档第 11 节：`{"schema":1,"actions":[{"id","target",
"after_event","delay_ms","action","args"}]}`。action 一次一结果（`scenario_result`
事件）；`after_event` 门控（ready/ap_ready/session_online/session_offline/null）；
`delay_ms` 0~60000。支持 `auto_provision`（连接本机配网服务完成 auth+wifi_config）、
`send_app_data`（>512 字节 → `rejected/payload_too_large`）、`inject_fault`、
`request_stop`。`auto_provision` 需配网 AP 已就绪（建议 `after_event=ap_ready`）。

## 协议产物与安装

```powershell
cmake --install out/device --config Debug --prefix out/device-artifact
```

产物：`bin/provision_device.exe`、`config/*.json`、
`share/protocol-contract.json`（与 golden 语义一致）、`share/manifest.json`。

## 自包含验证

将 `demo/device` 复制到任意临时目录后执行同样的 configure/build/ctest/offscreen
启动即可（本目录不含任何 `..` 以外的路径引用）。

## ESP32-C2（SS05 遗留，宿主构建不涉及）

`firmware/esp32c2/` 是独立 ESP-IDF 工程，`main/CMakeLists.txt` 直接组合
`backends/esp32c2/esp32c2_backend.c` 与设备平台无关 runtime 源码，cJSON 引用
`../../third_party/cjson`（device 目录内部）。`app_main` 不使用宿主 Qt 程序，
也不调用宿主后端 dispatcher。宿主构建仅对骨架做纯 C 语法/接口完整性检查
（tests/esp32），不得视为 ESP-IDF 构建通过。
