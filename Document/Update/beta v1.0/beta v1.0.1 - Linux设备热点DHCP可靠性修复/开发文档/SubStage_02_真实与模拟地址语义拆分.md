# SubStage 02：真实与模拟地址语义拆分

- 所属 Stage：Stage 1 - 纯逻辑与共用契约
- 依赖前置：无
- 并行状态：可与 SubStage 01、03 并行
- 所属阶段：阶段三 - 开发

## 1. 目标

明确 `192.168.1.1` 仅为模拟后端逻辑 AP 地址；真实 Linux 配网地址由运行时 AP 和 DHCP router 决定，同时保持模拟测试和端口翻译兼容。

## 2. 当前状态

- `demo/include/protocol.h:20` 定义 `PROTO_AP_IP "192.168.1.1"`。
- `demo/net_sim/sim_world.cpp:220` 返回固定模拟 AP 地址。
- `demo/协议文档.md:12` 把固定地址表述为统一真实地址。

## 3. 文件范围

允许：

- `demo/include/protocol.h`
- `demo/net_sim/sim_world.cpp`
- `demo/net_sim/sim_backend.cpp` 中模拟地址常量引用与真实模式端口日志
- `demo/tests/test_sim_world.cpp`
- `demo/tests/test_sim_socket.cpp`
- `demo/tests/test_provision.cpp`
- `demo/协议文档.md`
- `demo/设计文档.md`

禁止：

- `demo/pc/**`
- `demo/net_win/**`
- 修改模拟地址数值
- 修改 TCP/JSON 消息字段

## 4. 契约

```c
#define PROTO_SIM_AP_IP "192.168.1.1"
```

- 删除或完成迁移后移除 `PROTO_AP_IP`。
- 模拟后端和模拟测试只引用 `PROTO_SIM_AP_IP`。
- 真实 Linux 代码不得引用该常量。
- 协议文档定义真实配网端点为“设备 DHCP router 地址:5935”。
- 模拟端点仍为 `192.168.1.1:5935`，由后端翻译至 loopback 实际端口。

## 5. 附带修复

`demo/net_sim/sim_backend.cpp:398` 在真实模式仍打印模拟端口。真实模式日志必须显示 `PROTO_TCP_PORT`，模拟模式显示 `device_ap_port_base + index`；不得改变实际 socket 行为。

## 6. 测试

- 模拟 AP 地址仍等于原网络序值。
- 模拟地址翻译和完整配网测试通过。
- 全仓搜索不再存在 `PROTO_AP_IP`。
- 真实模式日志端口与实际监听 5935 一致。

## 7. 验收标准

- 不修改任何 PC 专有文件。
- 模拟地址、端口映射和多设备行为无回归。
- 文档不再宣称真实 Linux 固定使用 `192.168.1.1`。
