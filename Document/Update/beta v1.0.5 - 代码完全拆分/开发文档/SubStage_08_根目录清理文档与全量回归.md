# SubStage 08：根目录清理、文档与全量回归

- 所属 Stage：Stage 4
- 依赖前置：SubStage 02～07 全部完成并通过自测
- 并行状态：需等待，最后执行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

在双端自包含和 integration 已验证后，删除旧根级共享生产源码，更新导航文档和脚本，并执行阶段三全量回归及启动测试。

## 2. 文件所有权

本任务独占：

- `demo/README.md`
- 根 `README.md`
- 旧根共享目录和脚本的删除
- `Document/Update/beta v1.0.5 - 代码完全拆分/阶段三完成报告.md`
- 阶段三问题清单追加

不得重写已完成的端侧生产实现；发现缺陷应回派对应 SubStage 修复。

## 3. 删除前门禁

只有以下证据齐全才可删除旧根目录：

- PC 临时复制构建/单测/启动通过。
- 设备临时复制构建/单测/启动通过。
- integration 不含生产源码且 sim 场景通过。
- 两端 protocol manifest 与 golden 相等。
- 设备语言边界扫描通过。

否则保持旧根目录并报告，不得进行破坏性清理。

## 4. 清理范围

按 SS01 迁移清单删除旧 `demo/cmake`、`common`、`include`、`net_sim`、`net_win`、`net_linux`、`net_esp32c2`、`shared_ui`、`config`、`tests`、`third_party`、旧双端脚本和确认无用的空 `app/host/ui`。

不得删除两端新自包含目录、integration 或用户未纳入迁移清单的文件。

## 5. 文档

- 根 README：准确描述两个自包含产品和 integration，不再使用固定过时测试数量。
- PC README：只描述 PC 平台、配置、构建、测试、运行、artifact。
- 设备 README：区分 Qt 模拟器、Linux real 和 ESP32 骨架；明确 C++ 边界和硬件阻塞。
- integration README：只描述 artifact 输入、runner、场景和证据。
- 所有命令从对应产品目录可复制执行，不依赖旧根配置。

## 6. 全量验证

按验收文档执行当前环境可执行的：

- Windows PC/设备 Debug 构建与全量 CTest
- Qt offscreen 单端启动
- 双进程 sim integration
- 两端临时目录 isolation
- 静态源码/CMake/语言边界扫描
- ESP-IDF build（环境存在时）

Linux real、Windows real WiFi、ESP32 实机若缺环境，写阻塞证据，不能记通过。

## 7. 完成报告

记录 Git revision、环境、命令、退出码、测试数量、日志路径、artifact hash、启动操作、通过/失败/阻塞及未解决问题。不得写“全部通过”而无证据。

## 8. 禁止事项

- 禁止在验证前删除旧共享源码。
- 禁止修复无关问题或覆盖用户工作区改动。
- 禁止沿用 README 的历史“62 测试”口径。
- 禁止把阻塞项默认判通过。
