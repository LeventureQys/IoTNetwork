# Integration Tests 契约说明

本目录只保存跨端公开契约和测试脚本，**不含任何端侧生产源码**。

## 目录

- `protocol-contract.golden.json`：阶段二 SS01 冻结的双端协议契约。PC 与设备各自生成的 `share/protocol-contract.json` 必须与本文件在语义上完全一致。

## 使用规则

1. Integration runner 只允许启动两端已构建的可执行文件，禁止 `#include` 任何端侧私有头。
2. 禁止 `add_subdirectory(../pc)`、`add_subdirectory(../device)`，禁止编译两端 `.c/.cpp`。
3. 允许读取：可执行文件、端侧配置模板、`protocol-contract.json`、`manifest.json`、进程退出码、stdout/stderr、JSONL 事件文件和临时运行目录。
4. 每一场景使用独立临时目录，通过显式 `--sim-catalog-dir` 向两端传同一绝对目录。

## JSONL 事件

两端以 `--events-jsonl <path>` 输出，事件字段契约见 `Document/Update/beta v1.0.5 - 代码完全拆分/设计文档.md` 第 11 节。

## 场景

场景 JSON 以 `--scenario <path>` 传入；只允许 sim 后端。schema 和 action 定义见设计文档第 11 节。
