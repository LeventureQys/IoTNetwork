# SubStage 02：PC 端自包含工程

- 所属 Stage：Stage 2
- 依赖前置：SubStage 01
- 并行状态：可与 SubStage 03、04、05 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

将 `demo/pc/` 改造成可单独复制、配置、构建、测试和启动的 PC 产品目录。PC 允许继续使用 C++17/Qt；本任务不承担设备纯 C 化。

## 2. 当前状态

- `demo/pc/CMakeLists.txt:10` 通过 `DEMO_ROOT` 引用父目录。
- `demo/net_win/win_backend.cpp:2` 依赖模拟后端。
- `demo/pc/app/main.cpp:29` 默认配置依赖 CWD。
- Linux PC 当前只支持 sim，但共享 CMake 会编译设备热点代码。

## 3. 文件所有权

本任务独占：

- `demo/pc/**`

允许从旧根目录复制所需文件，但不得修改或删除旧根副本；删除由 SS08 负责。本任务不得修改 `demo/device/**`、`demo/integration_tests/**`（SS01 contract 除外）。

## 4. 目标目录和目标

按 `设计文档.md` 第 4、7 节建立 `include/pc`、`common`、`backends/socket|sim|windows`、`ui/components`、`config`、`scripts`、`tests`、`third_party`。

CMake 目标必须为：`pc_cjson`、`pc_common`、`pc_socket_backend`、`pc_sim_backend`、Windows 下 `pc_win_backend`、`pc_core`、`pc_ui`、`provision_pc` 和端侧测试目标。

禁止 `DEMO_ROOT`、PC 根外源码/include/add_subdirectory。第三方库不应用 `/WX`；Qt AUTOMOC 仅用于 UI。

## 5. 后端契约

- PC socket 目标只负责 TCP/UDP/time/random。
- sim 目标负责 PC 模拟扫描、连接、AP catalog 读取和 endpoint 翻译，不含 Linux 热点、设备 NVS和设备状态。
- Windows 目标实现 Native WiFi/IP/gateway，并组合 socket 目标；不得链接或创建 sim backend。
- Linux 构建只提供 sim，不编译 `linux_hotspot*` 或设备 STA。
- `net_ctx_create` 使用设计文档第 9 节新签名并传播 init 失败。

## 6. 配置与 CLI

提供 `config/pc_config.json` 和 default 模板。支持：

- 既有 `--config`、`--auto-provision`、`--duration`、`--sim`
- 新增 `--backend sim|windows`、`--runtime-dir`、`--sim-catalog-dir`、`--log-dir`、`--events-jsonl`、`--scenario`

`--sim` 等价于 `--backend sim`。Windows 默认 windows，Linux 默认 sim。CLI 相对路径以启动 CWD 规范化，配置内相对路径以配置目录解析；不 `chdir`，不向上搜索。配置缺失和字段容错保持现有默认值语义。

结构化事件和 scenario 严格遵循设计文档第 11 节。scenario 只允许 sim。

## 7. 协议产物

PC 构建必须生成或复制 `share/protocol-contract.json`，语义与 SS01 golden 相同；同时生成 artifact `manifest.json`。提供标准 install 规则：`cmake --install <pc-build> --config Debug --prefix <pc-artifact>`。不得从设备目录读取该文件。

## 8. 单元测试

迁入并按 PC 所有权调整：

- frame/protocol/log/net abstraction/PC params
- registry、host lifecycle
- PC sim world/socket/endpoint
- `test_host_app.cpp` 中纯 PC 输入和策略测试，使用 fake backend

不得编译 `device_app.c` 或包含设备私有头。新增测试覆盖配置不依赖 CWD、backend 初始化失败、Windows 不链接 sim、Linux 不携带热点、JSONL 写失败和 scenario 非 sim 拒绝。

## 9. 验收

- 从临时目录单独复制 `demo/pc` 后，Windows 配置/构建/CTest/Qt offscreen 启动通过。
- 在可用 Linux 环境执行同等 sim 构建和启动；缺环境记录阻塞。
- `rg` 检查无 `DEMO_ROOT`、`../device`、根共享生产路径和 `linux_hotspot`。
- Windows 二进制依赖和构建图不包含 `pc_sim_backend` 于 Windows 后端内部。

## 10. 禁止事项

- 禁止修改设备目录或根共享目录。
- 禁止宣称 Linux 真实 PC WiFi 已支持。
- 禁止改变协议和配网行为。
- 禁止用 symlink/junction 指向旧根源码。
