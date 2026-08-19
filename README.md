# ModuTech IoT Network

PC 热点直连拓扑（beta v1.1）的**设计 + 验证**一体化工程：Windows PC 主动创建 WPA2 移动热点，
Linux 设备扫描并连接热点后直连固定地址 `192.168.137.1:5935`，执行握手、心跳与应用数据。
包含完整的版本开发文档、可运行的 PC/设备双端 Demo、以及跨端黑盒集成测试。

面向**工厂局域网离线场景**：热点、TCP 连接、心跳保活、异常自愈全在局域网内完成，不依赖互联网。
beta v1.1 明确裁剪掉旧版设备 SoftAP 配网、mDNS/组播服务发现与多设备并发；ESP32-C2 不在本版本
开发与验收范围。

## 仓库结构

```
modutechnetwork/
├── 流程说明.md                    # 历史五阶段流程文档（beta v1.0 及更早；v1.1 拓扑见 demo README）
├── 历史主线.md                    # 版本沿革与迭代记录
├── figures/                      # 流程图 PNG（scripts/draw_flowcharts_v2.py 生成）
├── scripts/                      # 文档工具（流程图绘制、.md → .docx 转换）
├── archived/                     # 旧版归档
├── Document/                     # 版本开发过程文档 + 移植说明
│   ├── 移植说明/                 #   （历史交付件，v1.1 不再依赖）
│   └── Update/                   #   各版本开发记录（需求/问题清单/设计/验收）
└── demo/                         # 验证 Demo 工程
    ├── README.md                 #   ← 产品导航（v1.1 拓扑与联调）
    ├── pc/                       #   PC 上位机（自包含，C++17 + Qt6）
    ├── device/                   #   设备端（Qt 外壳 + 纯 C 核心 + 后端）
    └── integration_tests/        #   跨端集成测试（只消费构建产物）
```

`demo/pc` 与 `demo/device` 各自完全自包含，可单独复制到仓库外构建、测试与运行。

## 文档导航

### Demo 工程（v1.1 权威文档）

| 文档 | 内容 |
|------|------|
| [demo/README.md](demo/README.md) | Demo 产品导航、v1.1 拓扑与双端联调 |
| [demo/integration_tests/README.md](demo/integration_tests/README.md) | 跨端集成测试场景 A~H 与证据结构 |
| [demo/pc/README.md](demo/pc/README.md) | PC 上位机构建 / 运行 / 测试 |
| [demo/device/README.md](demo/device/README.md) | 设备端构建 / 运行 / 测试 / 语言边界 |

### 版本开发文档

| 文档 | 内容 |
|------|------|
| [Document/Update/beta v1.1/需求说明.md](Document/Update/beta v1.1/需求说明.md) | 配网流程简化需求 |
| [Document/Update/beta v1.1/设计文档.md](Document/Update/beta v1.1/设计文档.md) | 协议/配置/事件/测试设计 |
| [Document/Update/beta v1.1/验收文档.md](Document/Update/beta v1.1/验收文档.md) | 可观察验收标准 |

## 快速开始

要求：Windows 10/11、CMake ≥ 3.20、Visual Studio 2022（MSVC ≥ 19.30）、Qt 6。

```powershell
# PC 端
cmake -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="D:/Devtools/Qt/6.8.3/msvc2022_64" -DPC_BUILD_TESTS=ON `
  -S demo/pc -B out/pc-build
cmake --build out/pc-build --config Debug -j8
ctest --test-dir out/pc-build -C Debug --output-on-failure

# 设备端
cmake -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="D:/Devtools/Qt/6.8.3/msvc2022_64" -DDEVICE_BUILD_TESTS=ON `
  -S demo/device -B out/device-build
cmake --build out/device-build --config Debug -j8
ctest --test-dir out/device-build -C Debug --output-on-failure
```

完整联调与集成测试说明见 [demo/README.md](demo/README.md) 与
[demo/integration_tests/README.md](demo/integration_tests/README.md)。

## 验证状态（beta v1.1）

| 测试类型 | 用例数 | 状态 |
|----------|--------|------|
| PC 单元/UI/契约测试 | 146（120+3+23） | 通过 |
| 设备单元/组件测试 | 134 | 通过（runtime/config/core/sim/linux/esp32/ui） |
| 跨端集成测试（sim 双进程） | 8 场景（a–h） | 通过（契约一致/热点直连会话/断线重连/第二设备拒绝/无目标/应用数据/帧边界） |
| 真实环境（Windows 热点 / Linux 真实设备） | — | **阻塞**：本机无无线网卡/无 Linux 环境（见阶段三问题清单 CQ4/CQ7） |

真实 Windows 热点与 Linux 真实设备验收需专用硬件环境；sim 自动化不能替代真实环境结论，
当前按规范记录为阻塞项。

## 文档生成

```powershell
# 流程图（需 graphviz，输出到 figures/；历史文档用）
python scripts/draw_flowcharts_v2.py
```
