# SubStage：设备端日志与文档

- 所属工作线：设备端
- 所属 Stage：D2 - 设备日志落盘
- 依赖前置：无
- 并行状态：可与设备端SubStage 01并行，文件范围不重叠
- 所属阶段：阶段三 - 开发

## 1. 任务目标

让设备程序将公共诊断日志保存到 `demo/logs/`，同时保留终端输出且不重复，并同步设备端使用说明。

## 2. 文件范围

- `demo/device/app/main.cpp`
- `demo/tests/test_log.cpp`（可选新增）
- `demo/tests/CMakeLists.txt`（如新增测试）
- `demo/README.md` 设备段落
- `demo/协议文档.md` 设备端点段落

## 3. 实现契约

- 日志文件：`logs/provision_device_dev<index>_YYYYMMDD_HHMMSS.log`。
- 目录相对配置根目录。
- 复用 `log_set_file()`，不在启动脚本tee。
- 日志打开失败写stderr但不阻止启动。
- 初始化后的所有提前返回关闭日志文件。
- 正常退出关闭日志文件。

## 4. 测试与观察

- 公共日志文件写入标记恰好一次。
- sink不导致文件重复。
- 短时设备启动后存在对应日志并包含日志路径。
- 日志不得出现WiFi密码。

## 5. 禁止事项

- 不修改公共日志格式或增加第二日志框架。
- 不修改 `start_device.sh` 的sudo和参数语义。
- 不修改PC端文件。

## 6. 完成报告内容

- 日志路径示例。
- 启动/测试证据。
- sudo或现场网络阻塞说明。
