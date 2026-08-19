# ModuTech IoT Network

ESP32-C2 设备组网配网流程的**设计 + 验证**一体化工程。包含完整的五阶段流程文档、可运行的 PC/设备双端 Demo、以及 ESP32 硬件抽象层移植方案。

面向**工厂局域网离线场景**：配网、服务发现、TCP 连接、心跳保活、异常自愈全在局域网内完成，不依赖互联网。

## 仓库结构

```
modutechnetwork/
├── 流程说明.md                    # 核心设计文档：五阶段流程 + 协议 + 保障机制
├── 流程审核与重设计说明.md          # 旧版问题分析与重设计依据
├── 历史主线.md                    # 版本沿革与迭代记录
├── figures/                      # 流程图 PNG（scripts/draw_flowcharts_v2.py 生成）
├── scripts/                      # 文档工具（流程图绘制、.md → .docx 转换）
├── archived/                     # 旧版归档
├── Document/                     # 版本开发过程文档 + 移植说明
│   ├── 移植说明/
│   │   ├── MD/                   #   移植文档源文件（.md）
│   │   └── Word/                 #   移植文档交付件（.docx）
│   └── Update/                   #   各版本开发记录（需求/问题清单/设计/验收）
└── demo/                         # 验证 Demo 工程
    ├── README.md                 #   ← 产品导航
    ├── pc/                       #   PC 上位机（自包含，C++17 + Qt6）
    ├── device/                   #   设备端（Qt 外壳 + 纯 C 核心 + ESP32 骨架）
    └── integration_tests/        #   跨端集成测试
```

`demo/pc` 与 `demo/device` 各自完全自包含，可单独复制到仓库外构建、测试与运行。

## 文档导航

### 核心设计

| 文档 | 内容 |
|------|------|
| [流程说明.md](流程说明.md) | 五阶段主流程 + 协议报文 + 参数表 + 保障机制 + 附录 |
| [figures/](figures/) | 6 张流程图（设备启动/配网/发现/连接/上位机配网/连接保障） |

### 移植文档

| 文档 | 内容 |
|------|------|
| [ESP32移植指南](Document/移植说明/MD/ESP32移植指南.md) | 面向嵌入式工程师的任务清单（29 接口 + 流程验证） |
| [ESP32移植方案](Document/移植说明/MD/ESP32移植方案.md) | 硬件抽象层接口契约与关键约定值速查 |
| [配网流程说明](Document/移植说明/MD/配网流程说明.md) | 零代码配网流程 + 流程图 + 协议速查 |
| [验收说明](Document/移植说明/MD/验收说明.md) | 移植完成后的编译/接口/流程/异常四级自验清单 |

### Demo 工程

| 文档 | 内容 |
|------|------|
| [demo/README.md](demo/README.md) | Demo 产品导航与双端联调说明 |
| [demo/pc/README.md](demo/pc/README.md) | PC 上位机构建 / 运行 / 测试 |
| [demo/device/README.md](demo/device/README.md) | 设备端构建 / 运行 / 测试 / 语言边界 |
| [demo/device/backends/esp32c2/README.md](demo/device/backends/esp32c2/README.md) | ESP32-C2 骨架说明 |

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

完整联调说明见 [demo/README.md](demo/README.md)。

## 验证状态

| 测试类型 | 用例数 | 状态 |
|----------|--------|------|
| PC 单元测试 | 103 | 通过 |
| 设备单元/组件测试 | 117 | 通过（runtime / config / core / sim / linux / esp32 / ui） |
| 跨端集成测试 | 8 场景（a–h） | 通过（协议一致性 / 配网→会话 / 断线重连 / 帧边界 等） |
| 自包含验收 | — | PC 与设备分别复制到仓库外构建 + 测试通过 |

ESP32 实机验证需专用硬件环境，未在本仓库自动执行。

## 文档生成

```powershell
# 流程图（需 graphviz，输出到 figures/）
python scripts/draw_flowcharts_v2.py

# .md → .docx（读取 Document/移植说明/MD/，输出到 Document/移植说明/Word/）
python scripts/md_to_docx_v2.py
```
