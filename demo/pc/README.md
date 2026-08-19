# ModuTech PC（provision_pc）

PC 端自包含产品目录（beta v1.0.5 代码完全拆分）。可单独复制到仓库外构建、测试与运行，
不依赖父目录任何源码/配置/第三方库。PC 使用 C++17 + Qt。

## 目录

- `app/`：入口（CLI/路径/后端选择/JSONL 事件/scenario）
- `core/`：上位机核心（registry/tcp server/announcer/mDNS/provision client/host app）
- `backends/socket/`：TCP/UDP/非阻塞 socket/时钟/随机数
- `backends/sim/`：PC 模拟 WiFi、AP catalog 读取、endpoint 翻译
- `backends/windows/`：Windows Native WiFi/IP/网关（组合 socket 后端，不依赖 sim）
- `include/pc/`、`common/`：PC 自有公共 C（frame/log/net abstraction/params/protocol/事件/scenario/路径/SHA-256）
- `ui/`、`ui/components/`：Qt 窗口与 PC 自有组件（仅此处启用 AUTOMOC）
- `config/`：`pc_config.json` 与 `pc_config.default.json`
- `share/protocol-contract.json`：协议契约（与 integration golden 语义一致）
- `tests/`：gtest 单元测试
- `third_party/`：cJSON 与 googletest-1.15.2（不继承 /WX）

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
out/pc-build/bin/Debug/provision_pc.exe --backend sim --duration 3
```

CLI：`--config`、`--backend sim|windows`（`--sim` 等价）、`--auto-provision`、`--duration`、
`--runtime-dir`、`--sim-catalog-dir`、`--log-dir`、`--events-jsonl`、`--scenario`。
Windows 默认 windows 后端，Linux 默认 sim。配置缺失使用内置默认值并警告；
CLI 相对路径按启动 CWD 解析，配置内相对路径按配置目录解析；不 chdir、不向上搜索仓库。

## 安装产物

```powershell
cmake --install out/pc-build --config Debug --prefix out/pc-artifact
```

产物：`bin/provision_pc.exe`、`config/`、`share/protocol-contract.json`、`manifest.json`。
