# SubStage SS2.4：sim 双端热点角色反转

## SubStage: sim 双端热点角色反转
- 所属 Stage：Stage 2 - 端侧实现
- 依赖前置：SS1.1
- 并行状态：可与真实端业务并行，但会同时修改 PC/设备 sim 文件，由 MainAgent协调文件所有权
- 所属阶段：阶段三 - 开发

## 1. 目标

将旧“设备发布热点、PC扫描”的跨进程 sim 模型反转为“PC发布热点、设备扫描并连接”，保证自动化完整走新业务，不允许绕过扫描或热点启动。

## 2. 当前代码

- PC 读取 device schema1 catalog：`demo/pc/backends/sim/sim_backend.cpp:131`。
- PC sim 仍支持作为STA连接设备热点：`demo/pc/backends/sim/sim_backend.cpp:313`。
- 设备发布 schema1 AP：`demo/device/backends/sim/sim_backend.c:188`。
- 设备 TCP endpoint 将设备AP地址翻译：`demo/device/backends/sim/sim_backend.c:295`。

## 3. 文件所有权

允许修改两端 `backends/sim/`、对应 sim 单元测试和 sim README。不得修改 core、Windows、Linux、UI、integration runner、contract。

如 SS2.1 已修改 PC sim hotspot 文件，需基于其结果继续，禁止覆盖 HostApp lifecycle 逻辑。

## 4. schema 2

严格使用设计文档 7.3 的 `<catalog>/pc-hotspot.json` 字段。两端独立实现 validator。

PC：start 原子发布；status 查询；configure 更新逻辑IP；stop/销毁 owner安全删除。

设备：scan读取；STA验证SSID/password；gateway返回逻辑地址；固定目标 connect 翻译 loopback endpoint。

## 5. 错误语义

- 无记录/过期/owner死亡：scan不返回目标，connect→NO_AP_FOUND。
- SSID不符：NO_AP_FOUND。
- 密码不符：AUTH_FAIL 202。
- schema/字段/IP/端口非法：忽略记录并警告，不崩溃。
- catalog不可写：PC hotspot start失败。

## 6. 测试

1. PC发布完整schema2。
2. 原子写入和owner清理。
3. 设备scan发现。
4. 精确SSID和密码成功。
5. 错误密码202。
6. 过期/owner/schema/损坏忽略。
7. 192.168.137.1:5935翻译loopback。
8. 其他地址不错误翻译。
9. PC停止后设备不再扫描到热点。
10. 两个独立进程使用同目录，不依赖全局SimWorld共享内存。

## 7. 验收标准

- 不再创建 `device-<index>.json` 作为主流程。
- PC是唯一 catalog publisher。
- 设备在PC未启动时不能上线。
- 所有 sim 单元测试通过。

## 8. 禁止事项

不得共享生产源码；不得把密码写日志；不得为了旧测试保留双向发布主路径；不得修改Linux热点代码。

## 9. 完成报告

提供schema样例、文件清单、测试结果和并发/清理说明。