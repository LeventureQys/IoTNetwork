# ModuTech PC（provision_pc）

PC 端自包含产品目录（beta v1.1 一对一热点直连拓扑）。可单独复制到仓库外构建、测试与运行，
不依赖父目录任何源码/配置/第三方库。PC 使用 C++17 + Qt。

## 目录

- `app/`：入口（CLI/路径/后端选择/JSONL 事件/scenario）
- `core/`：上位机核心（registry / 一对一 tcp server / host app 生命周期）
- `backends/socket/`：TCP/UDP/非阻塞 socket/时钟/随机数
- `backends/sim/`：PC 模拟后端，发布 schema2 `pc-hotspot.json`（设计文档 7.3）
- `backends/windows/`：Windows 后端（现代移动热点 WinRT + 组合 socket 后端，不依赖 sim）
- `include/pc/`、`common/`：PC 自有公共 C（frame/log/net abstraction/params/protocol/事件/scenario/路径/SHA-256）
- `ui/`、`ui/components/`：Qt 窗口与 PC 自有组件（仅此处启用 AUTOMOC）
- `config/`：`pc_config.json` 与 `pc_config.default.json`
- `share/protocol-contract.json`：协议契约（与 integration golden 语义一致）
- `tests/`：gtest 单元测试
- `third_party/`：cJSON 与 googletest-1.15.2（不继承 /WX）

注：`core/host_announcer.*`、`core/host_mdns.*`、`core/host_provision_client.*` 为 beta v1.0 旧配网/发现业务，
文件保留但不参与编译。

## 构建（Windows，MSVC 2022 + Qt 6.8.3）

```powershell
cmake -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="D:/Devtools/Qt/6.8.3/msvc2022_64" `
  -DPC_BUILD_TESTS=ON -S . -B out/pc-build
cmake --build out/pc-build --config Debug --parallel 8
ctest --test-dir out/pc-build -C Debug --output-on-failure
```

## 运行

```powershell
$env:QT_QPA_PLATFORM="offscreen"   # 无头冒烟
out/pc-build/bin/Debug/provision_pc.exe --backend sim --duration 3 `
  --sim-catalog-dir <dir> --events-jsonl <file>
```

CLI：`--config`、`--backend sim|windows`（`--sim` 等价）、`--duration`、
`--runtime-dir`、`--sim-catalog-dir`、`--log-dir`、`--events-jsonl`、`--scenario`。
Windows 默认 windows 后端，Linux 默认 sim。配置缺失使用内置默认值并警告；
CLI 相对路径按启动 CWD 解析，配置内相对路径按配置目录解析；不 chdir、不向上搜索仓库。

Windows 后端启动顺序：创建移动热点 → 校验固定 IP `192.168.137.1/24`（先等待 ICS
自动分配，超时后强制设置）→ 监听 `0.0.0.0:5935`。任一步失败按逆序回滚并退出码 2；
不支持/无 WiFi 网卡/权限不足均 fail-closed，不回退 sim。

> 真实环境依赖：Windows 真实热点需要支持"移动热点"的无线网卡 + 驱动 + 管理员权限。
> 以 windows 后端（或默认）启动时，程序会自动请求 UAC 提权（非提权时以 `runas`
> 重新启动自身，sim 后端不要求提权）；用户取消授权则 fail-closed 退出码 2。
> 已实测（Realtek 8821CE，Win11 SDK 10.0.26100）：提权后移动热点启动成功，
> ICS 自动将承载适配器配置为 192.168.137.1/24，TCP 5935 正常监听，退出时热点可靠停止。

## 集成测试与启动测试

- 双进程 sim 集成场景 A~H 由 `../integration_tests/runners/run_scenario.ps1` 驱动
  （先启动 PC 并等待 `hotspot_ready`，再启动设备；PC 只以 `--backend sim` 参与）。
- 单进程启动冒烟（PC sim offscreen）：

```powershell
$env:QT_QPA_PLATFORM="offscreen"
out/pc-build/bin/Debug/provision_pc.exe --backend sim --duration 3 `
  --sim-catalog-dir out\cat --events-jsonl out\pc-events.jsonl
```

期望事件顺序：`hotspot_ready → tcp_listening → ready → shutdown_complete`，退出码 0，
结束后 `out\cat` 下无 `pc-hotspot.json` 残留。

## 安装产物

```powershell
cmake --install out/pc-build --config Debug --prefix out/pc-artifact
```

产物：`bin/provision_pc.exe`、`config/`、`share/protocol-contract.json`、`manifest.json`。
