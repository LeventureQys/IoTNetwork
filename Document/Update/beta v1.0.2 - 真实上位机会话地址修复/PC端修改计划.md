# beta v1.0.2 PC端修改计划

## 1. 目标

仅修改 PC 上位机，使真实 TCP listener 在任何服务通告之前就绪，启动失败立即回滚，并让日志准确区分 `0.0.0.0` 监听端点与局域网通告端点。该计划不修改设备端；当前 Linux 环境不要求执行 Windows真实运行测试。

## 2. 当前代码事实

- `demo/net_win/win_backend.cpp:397` 的 listener 已绑定 `INADDR_ANY`。
- `demo/pc/core/host_tcp_server.cpp:34` 日志却显示 `host_virtual_ip`，与实际bind不一致。
- `demo/pc/core/host_app.cpp:21` 当前顺序为 mDNS、announcer、listener。
- `HostTcpServer` 没有公开 `Stop()`；listener主要靠析构关闭。
- `HostMdns::Register()` 返回 `void`，真实Windows后端仍委托进程内 `SimWorld`，不是局域网真实mDNS。
- `HostAnnouncer` 的UDP组播才是本版本真实服务发现主路径。
- `win_backend_get_ipv4()` 选择第一个Up且非loopback IPv4，未排除APIPA。

## 3. Stage P1：监听生命周期

### 3.1 HostTcpServer接口

在 `demo/pc/core/host_tcp_server.h` 增加：

```cpp
void Stop();
bool Started() const { return listen_ != nullptr; }
```

契约：

- `Start()` 在已有listener时返回 `DEMO_OK` 或明确的 `DEMO_ERR_INVAL`，实现者必须选择一种并用测试固定；推荐幂等成功。
- `Stop()` 关闭pending、online连接和listener；可重复调用。
- 析构调用 `Stop()`，不再复制关闭逻辑。
- `Stop()` 后 `Started()==false`。

### 3.2 Windows listener

保持：

```cpp
address.sin_addr.s_addr = htonl(INADDR_ANY);
address.sin_port = htons(port);
```

补充：

- socket创建、setsockopt、bind、listen和非阻塞设置失败日志，包含 `WSAGetLastError()`。
- 任一步失败关闭socket且不设置输出句柄。
- listener成功日志显示 `0.0.0.0:<port>`，不得写成只绑定 `host_virtual_ip`。
- 不在本版本引入 `SO_EXCLUSIVEADDRUSE` 行为变化；保留现有快速重启语义。

### 3.3 HostApp启动顺序

固定：

```text
tcp_server_.Start()
  -> announcer_.Start()
  -> mdns_.Register()（兼容、非关键）
  -> started_=true
```

错误处理：

- TCP失败：直接返回，announcer和mDNS均未启动。
- announcer失败：调用 `tcp_server_.Stop()` 后返回。
- mDNS失败：记录警告但不回滚TCP和组播；真实发现不依赖当前模拟mDNS。

`RequestStop()` 顺序：

```text
SendBye -> announcer.Stop -> tcp_server.Stop -> mdns.Unregister -> stop=true
```

`ForceCrash()` 不发送bye，但必须停止announcer、TCP和mDNS。

## 4. Stage P2：通告地址与日志

### 4.1 地址有效性

`win_backend_get_ipv4()` 候选必须拒绝：

- `0.0.0.0/8` 或 unspecified。
- `127.0.0.0/8`。
- `169.254.0.0/16`。
- 非 `AF_INET`、adapter非Up、软件loopback。

返回首个符合条件的IPv4。本版本不实现路由metric、VPN过滤、WLAN GUID绑定或动态刷新。

### 4.2 单一地址来源

真实PC启动仅调用一次 `win_backend_get_ipv4()`，结果写入 `params.host_virtual_ip`。以下组件只读该值：

- `HostAnnouncer` 的 `host_announce.ip`。
- `HostMdns` 的兼容服务结构。
- `HostApp` 和main启动日志。

不得由子组件重新枚举网卡。

### 4.3 日志契约

启动成功至少打印：

```text
运行模式：Windows真实
TCP监听端点：0.0.0.0:5935
服务通告端点：<host_virtual_ip>:5935
组播端点：224.0.2.1:5936
```

`HostAnnouncer::Poll()` 的发送日志增加IP和TCP端口，但不得打印WiFi密码。

### 4.4 mDNS边界

- 可将 `HostMdns::Register()` 改为 `int`，用于记录成功/失败状态。
- 继续使用现有抽象，不新增Bonjour、Windows DNS-SD或第三方库。
- README和验收文档必须说明：Windows真实发现的本版本主路径是UDP组播，不以真实mDNS为验收项。

## 5. Stage P3：PC端测试

### 5.1 当前环境可执行

利用模拟后端为 `HostApp` 增加启动顺序和回滚测试。推荐通过可控 fake backend 统计：

- TCP listen先于mcast join。
- TCP失败时未调用mcast/mdns。
- mcast失败时listener被关闭。
- Stop重复调用无崩溃、无重复关闭副作用。
- announce JSON使用 `params.host_virtual_ip` 和 `host_tcp_port`。

如现有模拟后端不支持注入启动失败，可在测试内构造最小 `net_backend_t` fake；不要为测试修改生产公开协议。

### 5.2 Windows待执行

- MSVC构建PC工程和测试工程。
- 启动真实 `provision_pc.exe`。
- 使用 `Get-NetTCPConnection` 或 `netstat -ano` 验证PID监听 `0.0.0.0:5935`。
- 捕获UDP 5936并核对announce IP/端口。
- 验证端口占用时程序启动失败且无announce。
- 验证无有效IPv4时程序拒绝启动。

当前Linux未执行这些项时必须标记“待Windows环境”，不影响PC代码提交检查，但不能宣告PC端实机验收通过。

## 6. 允许修改

- `demo/pc/app/main.cpp`
- `demo/pc/core/host_app.cpp`
- `demo/pc/core/host_app.h`
- `demo/pc/core/host_tcp_server.cpp`
- `demo/pc/core/host_tcp_server.h`
- `demo/pc/core/host_announcer.cpp`
- `demo/pc/core/host_announcer.h`
- `demo/pc/core/host_mdns.cpp`
- `demo/pc/core/host_mdns.h`
- `demo/net_win/win_backend.cpp`
- `demo/tests/test_host_app.cpp` 或新增PC生命周期测试
- `demo/README.md` 中PC监听与发现部分
- `demo/协议文档.md` 中PC通告部分

## 7. 禁止修改

- `demo/device/**`
- `demo/net_sim/sim_backend.cpp` 的设备真实TCP目标修复
- Linux热点、DHCP、NAT代码
- TCP/JSON协议字段
- 新增真实mDNS依赖
- 新增网卡选择UI
- 自动修改Windows防火墙

## 8. PC端完成标准

- 代码证明listener先于announcer启动。
- listener或announcer失败具有即时回滚。
- 日志准确区分监听和通告端点。
- 无效IPv4被过滤。
- 当前环境可执行的核心测试通过。
- Windows专有构建与运行结果明确为通过或待验收。
- 设备端即使尚未修改，PC端代码也可单独提交检查。
