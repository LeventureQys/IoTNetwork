# SubStage SS2.1：PC 热点后端与生命周期

## SubStage: PC 热点后端与生命周期
- 所属 Stage：Stage 2 - 端侧实现
- 依赖前置：SS1.1
- 并行状态：可与设备端 SS2.3 并行；与 SS2.2 同属 PC 文件域，建议同一 Agent 串行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

Windows PC 必须主动创建热点，确保热点接口为 `192.168.137.1/24` 后才监听 TCP 5935。sim 后端必须提供同等生命周期。失败时逆序回滚且不回退 sim。

## 2. 当前代码状态

- 公共 vtable：`demo/pc/include/pc/net_abstraction.h:20`。
- Windows 热点当前不支持：`demo/pc/backends/windows/win_backend.cpp:298`。
- HostApp 当前启动 TCP/announce/mDNS：`demo/pc/core/host_app.cpp:21`。
- PC 入口先读取任意局域网 IP：`demo/pc/app/main.cpp:341`。
- core 构建包含旧业务：`demo/pc/CMakeLists.txt:95`。

## 3. 文件所有权

允许修改：PC net abstraction、`backends/windows/`、`backends/sim/` 热点生命周期、`core/host_app.*`、`app/main.cpp`、`demo/pc/CMakeLists.txt`、相关 PC 测试和 README。

禁止修改设备端、integration runner、contract 值、ESP32。

## 4. 接口契约

等待 SS1.1 后读取 `demo/pc/include/pc/protocol.h` 和 `params.h`。实现设计文档 7.1 的：

```c
int net_wifi_ap_status(net_ctx_t *, net_ap_status_t *);
int net_wifi_ap_configure_ipv4(net_ctx_t *, const char *, int);
```

新增 `win_hotspot.h/.cpp` 的 `WinHotspot` 类，接口见设计文档 7.2。不得向公共 C 头暴露 WinRT 类型。

## 5. 生命周期

`HostApp::Start()`严格执行：热点 start→status→必要时 configure IPv4→再次 status→TCP start。成功产生 `hotspot_ready`。任一步失败：关闭 TCP（若有）→停止热点→返回原错误。

`RequestStop`/析构幂等：停止 TCP→停止热点。移除 announcer、mDNS、provision client、continuous provision 线程和 API。

入口不再获取任意当前 IPv4，不再创建配网线程或 `--auto-provision` 路径。Windows 后端失败直接退出码 2。

## 6. Windows 实现约束

- 只用现代 Windows 10/11 移动热点/WinRT 能力。
- 不允许 `netsh wlan hostednetwork` 回退。
- SSID/密码由配置传入；日志只允许 SSID。
- 找到实际热点承载适配器后读取/配置 IPv4。
- IP 设置必须限定目标热点适配器，禁止修改其他网卡。
- 能力缺失、权限、超时必须映射为明确错误并 fail-closed。

若当前工具链无法编译所选现代 API，停止并报告 MainAgent，不能自行改用 deprecated 路线。

## 7. sim 实现

PC sim `wifi_ap_start` 发布 `<catalog>/pc-hotspot.json` schema 2；status 返回目标值；configure IPv4 更新记录或内存状态；stop 按 owner_pid 删除。写入使用临时文件+原子 rename。

## 8. 测试

1. net abstraction NULL/未实现接口。
2. HostApp 热点/IP正确→TCP启动。
3. IP不正确→configure→成功。
4. configure失败→热点停止、TCP未启动。
5. TCP端口失败→热点停止。
6. Stop重复调用。
7. sim catalog 发布/清理/owner保护。
8. Windows `WinHotspot` 使用可注入 ops 覆盖成功、能力缺失、权限、超时。
9. PC offscreen sim 启动 3 秒，事件含 hotspot_ready、ready、shutdown_complete。

## 9. 验收标准

- HostApp 无 announcer/mDNS/provision 成员。
- Windows 不支持时明确失败，不回退 sim。
- `hotspot_ready` 先于 TCP ready。
- 失败无 catalog、热点或监听残留。
- 测试通过，日志无密码。

## 10. 禁止事项

不得改协议值；不得修改设备文件；不得保留旧配网入口；不得为测试在 HostApp 中绕过 backend；不得宣称普通 fake-ops 单测等价于真实 Windows 热点启动。

## 11. 完成报告

列出修改文件、系统 API、错误映射、测试结果及真实 Windows 环境是否可用。