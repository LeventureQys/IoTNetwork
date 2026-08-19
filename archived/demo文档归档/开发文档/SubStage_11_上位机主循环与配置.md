# SubStage_11：上位机主循环与配置（host_app + host_config）

## SubStage: 上位机主循环与配置
- 所属 Stage：Stage 4
- 依赖前置：SubStage_09、SubStage_10（注册表/TCP 服务器/通告/mDNS/配网客户端）
- 并行状态：需等待 SubStage_09/10；与 Stage 5 衔接
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

实现 HostApp（上位机主循环，poll 所有 I/O 源 + 优雅退出）与 host 侧配置封装。主循环驱动：TCP 服务器（注册/心跳）、组播通告（1s）、mDNS 注册、配网流程触发；退出时先 host_bye 再关闭全部连接。

## 2. 当前代码状态

- 已有：SubStage_01~10。
- 依据：`设计文档.md` 4.15（HostApp 契约）、2.2（模块架构）；`协议文档.md` 5.2（host_bye 时序）；`流程说明.md` 3.5.3（40：上位机重启恢复）。

## 3. 交付物（全部新建，C++17）

```
demo/host/host_app.h
demo/host/host_app.cpp
demo/host/host_config.h
demo/host/host_config.cpp
```

## 4. 接口契约

```cpp
// host_app.h
class HostApp {
public:
    HostApp(const demo_params_t& params, net_ctx_t* net);
    ~HostApp();
    int  Start();                       // 注册 mDNS + 通告 Start + TCP Start
    void Run();                         // 主循环（阻塞，直到 stop）
    void RequestStop();                 // 优雅退出：SendBye → BroadcastCloseAll → stop
    const HostRegistry& registry() const;
    HostRegistry& registry_mut();
    void SetBusyOverride(int limit);    // busy_inject 透传
    void ProvisionAllDevices();         // 扫描并串行配网所有 Modu_XXXX 热点（每台依次）
    // 剧本/测试查询
    size_t OnlineCount() const;
    uint64_t announce_seq() const;
};

// host_config.h（薄封装，可省：直接使用 params_load）
// —— 决议：不创建 host_config，配置加载统一用 params_load；本文件不交付。
```

## 5. 实现细节

### 5.1 Start

1. `net_mdns_register`（HostMdns::Register）。
2. `announcer_.Start()`；`tcp_server_.Start()`（失败 → 日志 ERROR + 返回 DEMO_ERR）。
3. 日志 `[HOST] host started, virtual_ip=..., tcp_port=..., mcast=...`。

### 5.2 Run（主循环）

```text
while (!stop_) {
    now = net_time_ms(net_);
    announcer_.Poll(now);          // 1s 通告
    tcp_server_.Poll(now);         // accept/收包/心跳监控
    Sleep(10);                     // 10ms 节拍（sim 环境；真实环境可 select）
}
```

- 配网触发：HostApp 不在 Run 内主动配网（由 main 在启动后调用 `ProvisionAllDevices` 一次性完成——配网是阻塞流程，放在 host 线程内由 main 线程调用会阻塞 main；**决议：main 在启动 host 线程前先调用 ProvisionAllDevices（此时 host 线程未启动，配网阻塞在 main 线程可接受，配网完成后启动 host 线程**。若存在多设备：串行配网完成后 host 启动，设备们同时进入发现 → 连接）——更简单且确定：**ProvisionAllDevices 由 main 线程调用（阻塞），完成后 main 再启动 host 线程**。配网期间 device 处于 AP_PROVISION，配网完成后 device 进入 DISCOVERY，此时 host 已启动（组播/mDNS/TCP 就绪）→ 快速发现窗口内连接成功。时序确定。
- 优雅退出：`RequestStop()`：`announcer_.SendBye()` → `tcp_server_.BroadcastCloseAll()` → `stop_=true`。

### 5.3 生命周期

- 析构：Unregister mDNS、Announcer Stop、TCP 关闭全部。

## 6. 验收标准

1. 构建通过，零警告。
2. 单元测试 `test_host_app.cpp`（sim 后端 + 模拟 device）：
   - Start 后：mDNS 可被解析、组播可收到 announce、TCP 端口可连接。
   - 模拟设备完整流程：配网（ProvisionAllDevices 单台）→ device hello → ack → 心跳若干 → host 侧 online 计数正确。
   - RequestStop：收到 host_bye（device 侧组播可收到）；所有连接关闭。
3. 与 device 集成（test_device_sm 联动）：多设备（2 台）配网 + 注册 + 心跳 → OnlineCount==2。

## 7. 禁止事项

- 不实现线程编排（Stage 5 main 负责创建 host/device 线程）。
- 不实现剧本（Stage 5）。
- 不直接调用 Winsock。
- 配网流程不得放入 Run 主循环（阻塞设计，见 5.2）。

## 8. 依赖前置

- 等待 SubStage_09、10 完成；本任务书完成后 Stage 4 结束，进入 Stage 5（SubStage_12）。
