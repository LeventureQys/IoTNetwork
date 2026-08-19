# beta v1.0.0 TCP/DHCP 配网问题修复说明

## 1. 本次需求

检查当前项目 PC 端程序日志，并联合检查 Windows PC 端与 Linux 设备端代码，定位并修复首次配网流程中找不到设备端 TCP 服务器地址的问题。

问题集中在以下链路：

1. Linux 设备启动 `Modu_XXXX` SoftAP。
2. Linux 设备通过 `dnsmasq` 向 PC 分配 DHCP 地址和默认网关。
3. Windows PC 关联设备热点。
4. Windows PC 从当前热点对应 WLAN 接口取得 DHCP 默认网关。
5. Windows PC 将该网关作为设备配网服务器地址，连接 TCP `5935`。
6. PC 与设备执行 `auth -> wifi_config -> wifi_result -> close_ap`。

本次不要求在当前 Windows 开发机执行 Linux 实机测试。Linux 设备上的编译、热点、DHCP 和 TCP 联调由用户后续完成。

## 2. 根因结论

本问题不是单独的 TCP 连接问题，而是 Linux DHCP 启动状态和 Windows 网关发现逻辑同时存在缺陷。

### 2.1 Linux 设备端根因

原设备端存在“热点假成功”问题：

- `hostapd` 成功后，即使 AP 网关地址配置失败，程序仍继续执行。
- `dnsmasq` 启动失败只记录日志，热点启动函数仍返回成功。
- 上层因此打印“设备热点已启动”，但 PC 实际收不到 DHCP 地址或默认网关。
- PC 最终可能没有 IPv4，或只能取得 `169.254.x.x` APIPA 地址，无法找到设备服务器。
- 原 `dnsmasq` 同时启用 DNS 53 端口；配网只需要 DHCP，但 DNS 端口冲突可能导致整个 `dnsmasq` 启动失败。
- DHCP router option 3 未显式配置，PC 依赖默认网关时存在不确定性。
- 配置文件使用 `linux_hotspot_config`，代码读取 `hs_config_path`，自定义热点配置路径实际不生效。
- Linux 热点实际地址原为 `10.42.0.1`，协议和模拟环境使用 `192.168.1.1`，两套地址契约不一致。

### 2.2 Windows PC 端根因

原 PC 端存在网关枚举和接口选择问题：

- `GetAdaptersAddresses()` 未设置 `GAA_FLAG_INCLUDE_GATEWAYS`，不能可靠填充 `FirstGatewayAddress`。
- 网关查询没有绑定到刚刚连接设备热点的 WLAN GUID。
- 多网卡、VPN、以太网、双 WiFi 或虚拟网卡环境下，可能返回其他接口的网关。
- 原逻辑在 WLAN 网关不存在时会回退到任意其他网卡网关，可能错误连接办公路由器或 VPN 网关的 `5935`。
- PC 只等待本机 IPv4 就绪，没有等待目标 WLAN 的 DHCP 网关就绪，存在 DHCP 地址与路由信息更新竞态。

## 3. 修复后的地址和服务契约

beta v1.0.1 后，真实 Linux 热点地址与模拟地址不再共用固定常量。真实热点按路由冲突检测动态选段，PC 仍以 DHCP router 作为配网服务器地址：

| 项目 | 当前值 |
|---|---|
| 真实 Linux 首选 | AP `10.42.0.1/24`；池 `10.42.0.100` - `10.42.0.200` |
| 真实 Linux 备用顺序 | `10.43.0.1/24` → `172.31.250.1/24` → `192.168.250.1/24` |
| 备用 DHCP 池 | 保留首选池 host offset；默认均为对应备用网段的 `.100` - `.200` |
| DHCP router option 3 | 最终选中的 AP 地址 |
| DHCP server identifier option 54 | 最终选中的 AP 地址 |
| 配网 TCP 服务 | DHCP router`:5935` |
| Linux TCP bind | `0.0.0.0:5935` |
| 模拟设备 AP 地址 | `192.168.1.1:5935` |

候选选择前会检查本机有效 IPv4 直连路由，使用第一个不冲突的候选。配置首选可调整；三个自动备用固定为上述 `/24`。PC 真实模式仍从目标 WLAN 动态取得 DHCP router，不直接硬编码真实地址。`192.168.1.1:5935` 仅用于模拟后端逻辑地址及 loopback 端口翻译，真实 Linux 不引用该模拟地址常量。

### 3.1 启动、readiness 与部署边界

- 配置 fail-closed：配置文件缺失、JSON/字段非法、工作目录不可访问时初始化失败并清空无效配置；不会使用部分配置继续启动，也不会回退到模拟网络。
- 地址规划 fail-closed：路由采集失败、路由结果不完整或所有首选/备用候选冲突时拒绝启动。
- 运行时 fail-closed：权限、依赖、接口所有权、hostapd、AP 模式、AP IPv4 回读、dnsmasq 或 UDP 67 readiness 任一失败，均回滚本次事务且不报告热点 ready。
- readiness 要求：hostapd PID 对应预期进程、接口处于 AP 模式、最终地址/掩码回读一致；dnsmasq PID 对应预期进程，且其 socket 已监听最终 AP 地址或通配地址的 UDP 67。配网 TCP 服务监听 `0.0.0.0:5935`，客户端使用 DHCP router`:5935`。
- 单实例：真实 Linux 模式持有 `${work_dir}/modu_linux_hotspot.lock`；锁被占用时后续实例拒绝初始化，停止并 deinit 后释放。
- 防火墙责任：INPUT/OUTPUT/FORWARD 过滤策略由部署环境负责，应用不修改；当前部署必须放行 `ap0` UDP 67 和 TCP 5935。
- NAT 默认关闭：`nat=false` 时不调用 iptables，本地 DHCP/TCP 配网不依赖 NAT。显式启用要求部署环境已设置 `ip_forward=1` 并处理 FORWARD 放行；应用只添加和清理自身带标识、源为最终 AP CIDR、出口为 STA 的精确 MASQUERADE 规则。
- 端到端条件：真实无线客户端必须完成 DHCP `DISCOVER/OFFER/REQUEST/ACK`，取得最终池内地址和最终 AP router，并能连接 router`:5935`；仅进程启动或界面显示热点成功不算通过。

## 4. 本次代码修改

### 4.1 网络抽象层

修改文件：

- `demo/include/net_abstraction.h`
- `demo/common/net_abstraction.c`

改动：

- 在 `net_backend_t` 中增加 `wifi_get_gateway`。
- 增加统一接口 `net_wifi_get_gateway()`。
- 网关值使用网络字节序，与 `net_addr_t.ip` 保持一致。
- 空上下文、空后端或未实现函数时安全返回 `DEMO_ERR_INVAL`，并将输出地址清零。

该修改避免 PC 业务代码直接调用 Windows 私有函数，使网关发现能力保持在网络抽象层内。

### 4.2 Windows PC 后端

修改文件：

- `demo/net_win/win_backend.cpp`
- `demo/net_win/win_backend.h`

改动：

- 删除无上下文的全局 `win_backend_get_gateway_ipv4()`。
- 使用 `wlan_connect()` 保存的 `connected_interface` GUID。
- 将目标 GUID 转换为 `NET_IFINDEX`。
- `GetAdaptersAddresses()` 增加 `GAA_FLAG_INCLUDE_GATEWAYS`。
- 只读取刚刚连接设备热点的目标 WLAN 接口网关。
- 删除回退到其他 WLAN、以太网、VPN 或虚拟网卡网关的逻辑。
- 过滤 `0.0.0.0`、loopback、APIPA 和 multicast 地址。
- 网关尚未写入系统路由信息时返回 `DEMO_ERR_AGAIN`。
- 日志增加目标 WLAN `IfIndex` 和最终 DHCP 网关。

### 4.3 PC 配网客户端

修改文件：

- `demo/pc/core/host_provision_client.cpp`

改动：

- 通过 `net_wifi_get_gateway()` 获取设备服务器地址。
- 连接热点后最多等待 10 秒，每 500 ms 查询一次目标 WLAN 网关。
- 只有取得非零有效网关后才开始 TCP `5935` 连接。
- 超时时明确提示检查设备端 DHCP option 3 和 `dnsmasq` 状态。
- 不再依赖 Windows 私有网关函数。

### 4.4 Linux 设备热点与 DHCP（beta v1.0.1 现状）

修改文件：

- `demo/net_linux/linux_hotspot.c`
- `demo/net_linux/linux_hotspot.h`
- `demo/config/linux_hotspot.json`

改动：

- 默认首选改为 `10.42.0.1/24`，默认池为 `10.42.0.100` - `10.42.0.200`。
- 增加按本机有效 IPv4 直连路由冲突检查，并依次尝试 `10.43.0.1/24`、`172.31.250.1/24`、`192.168.250.1/24` 三个固定备用段。
- DHCP router option 3 和 server identifier option 54 均使用最终选中的 AP 地址。
- `enable=0` 时返回失败，不再报告热点启动成功。
- 配置、路由采集、候选选择和运行时步骤均 fail-closed。
- AP 地址配置失败时立即清理 `hostapd`、`dnsmasq` 和 AP 接口，并返回失败。
- 将 `dnsmasq` 改为 DHCP-only，使用 `--port=0` 禁用 DNS 53 端口。
- 增加 `--dhcp-authoritative`。
- hostapd/AP 地址/dnsmasq readiness 全部通过后才设置热点 active；dnsmasq 还需验证所属进程确实监听 UDP 67。
- 增加真实模式单实例锁和按资源所有权逆序回滚。
- NAT 默认关闭；应用不修改 INPUT/OUTPUT/FORWARD 过滤规则。

### 4.5 模拟后端与真实监听日志

修改文件：

- `demo/net_sim/sim_backend.cpp`

改动：

- 模拟后端实现 `wifi_get_gateway`，连接模拟设备热点后返回 `192.168.1.1`。
- Linux 真实设备模式的 TCP 监听日志改为 `0.0.0.0:5935`。
- 模拟模式继续显示 `127.0.0.1:<模拟端口>`。

### 4.6 ESP32-C2 后端兼容

修改文件：

- `demo/net_esp32c2/esp32c2_backend.c`

改动：

- 为新增的抽象层接口补充占位实现。
- 当前 ESP32-C2 骨架返回 `DEMO_ERR`，避免 vtable 字段错位。

### 4.7 配置路径修复

修改文件：

- `demo/common/params.c`
- `demo/config/demo_config.json`
- `demo/config/demo_config_default.json`

改动：

- 正式配置键统一为 `hs_config_path`。
- 保留对旧键 `linux_hotspot_config` 的兼容读取，避免旧部署配置立即失效。

### 4.8 测试修改

修改文件：

- `demo/tests/test_net_abstraction.cpp`
- `demo/tests/test_params.cpp`

新增覆盖：

- 网关接口在空上下文和空 vtable 下安全返回。
- 模拟 host 连接设备 AP 后可取得设备 AP 虚拟网关。
- `hs_config_path` 可从 JSON 正确覆盖。

## 5. Windows 侧验证结果

当前开发机已完成：

- Visual Studio 2022 x64 Debug 配置成功。
- PC 核心、设备共享核心、模拟后端和测试程序编译成功。
- 本次相关 15 项定向测试全部通过。
- 排除仓库既有独立失败项 `HostE2E.WifiDisconnectReconnect` 后，其余 66 项测试全部通过。
- `git diff --check` 通过，无空白错误。

全量测试中仍存在一个与本次 TCP/DHCP 地址发现无关的既有问题：

- `HostE2E.WifiDisconnectReconnect` 在目标 WiFi 短暂断开后，设备状态机会清空凭据并重新进入配网模式，测试期望自动重连，因此失败。
- 本次没有修改该状态机行为，避免扩大需求范围。

## 6. Linux 设备端检查步骤

### 6.1 构建前检查

确认以下程序存在：

```bash
command -v hostapd
command -v dnsmasq
command -v iw
command -v ip
command -v nmcli
```

确认配置中的 STA 接口真实存在：

```bash
iw dev
ip link show
```

当前默认配置为：

```json
"sta_interface": "wlP2p33s0",
"ap_interface": "ap0",
"subnet": "10.42.0.1",
"prefix_length": 24,
"dhcp_start": "10.42.0.100",
"dhcp_end": "10.42.0.200",
"nat": false
```

如果设备网卡名不同，必须修改 `demo/config/linux_hotspot.json`。

### 6.2 启动要求

真实热点需要 root 权限：

```bash
sudo <device_program> <arguments>
```

主配置应包含：

```json
"use_real_wifi_sta": 1,
"hs_config_path": "config/linux_hotspot.json"
```

### 6.3 热点启动后检查

```bash
iw dev ap0 info
ip -4 addr show dev ap0
ss -lntp | grep ':5935'
ss -lunp | grep ':67'
ps -ef | grep -E 'hostapd|dnsmasq'
```

预期：

- `ap0` 类型为 AP。
- `ap0` 地址为本轮最终选中的地址；无冲突时为 `10.42.0.1/24`。
- TCP 正在监听 `0.0.0.0:5935`。
- `dnsmasq` 正在监听 DHCP UDP 67。
- `dnsmasq` 不需要监听 DNS 53。

### 6.4 DHCP 抓包检查

```bash
sudo tcpdump -ni ap0 -vvv 'udp port 67 or udp port 68'
```

PC 连接热点后应看到：

```text
DHCPDISCOVER
DHCPOFFER
DHCPREQUEST
DHCPACK
```

重点确认 OFFER/ACK：

- 分配地址位于最终选中网段的 DHCP 池；无冲突时为 `10.42.0.100` - `10.42.0.200`。
- 子网掩码为 `/24`。
- Router option 3 为最终 AP 地址。
- Server identifier option 54 为最终 AP 地址。

### 6.5 Windows PC 联调检查

PC 连接 `Modu_XXXX` 后执行：

```powershell
ipconfig /all
route print -4
Test-NetConnection <DHCP-router> -Port 5935
```

预期：

- PC WLAN 地址不是 `169.254.x.x`。
- PC WLAN 地址位于最终 DHCP 池。
- 默认网关为最终 AP 地址。
- TCP DHCP router`:5935` 可连接。
- PC 日志显示从目标 WLAN `IfIndex` 取得与最终 AP 地址一致的 DHCP 网关。

## 7. 建议重点检查的日志

### 7.1 Linux 设备端成功日志

应依次出现：

```text
TCP 正在监听 0.0.0.0:5935
创建虚拟接口
真实热点已启动
网关=<最终 AP 地址>
设备热点已启动
```

如果 AP 地址或 DHCP 启动失败，现在应出现明确错误并回滚，不应再出现最终“设备热点已启动”。

### 7.2 Windows PC 成功日志

应依次出现：

```text
已关联 SSID=Modu_XXXX，等待 DHCP/路由就绪
SSID=Modu_XXXX 网络就绪，本机 IPv4=<最终 DHCP 地址>
已从目标 WLAN 接口 IfIndex=... 获取 DHCP 网关=<最终 AP 地址>
配网：设备热点网关=<最终 AP 地址>（来自 DHCP）
配网：TCP 连接设备 <最终 AP 地址>:5935
```

### 7.3 常见失败判断

| 现象 | 优先检查 |
|---|---|
| PC 获得 `169.254.x.x` | `dnsmasq`、UDP 67、AP 地址、DHCP 抓包 |
| PC 有最终池内地址但无网关 | DHCP option 3、Windows 路由表 |
| PC 网关正确但 TCP 失败 | 设备 TCP 监听、防火墙、端口占用 |
| PC 日志显示其他网关 | 确认运行的是本次新构建 PC 程序 |
| 设备日志显示热点启动失败 | root、接口名、PHY 并发能力、hostapd/dnsmasq 依赖 |

## 8. 本次未处理范围

- 本文档同步不代表已完成 Linux 实机验证；实测结果以 MainAgent 后续阶段报告为准。
- 未修改目标 WiFi 断开后的凭据清除与自动重连状态机。
- 未增加 PC 配网 TCP socket 的显式 `IP_UNICAST_IF` 绑定；当前已确保服务器地址来自正确 WLAN 接口，后续如现场存在相同网段多接口冲突，可继续增加 socket 出口接口绑定。
- 未修改 `auth/wifi_config/close_ap` 协议格式。

## 9. 本次修改文件清单

```text
demo/common/net_abstraction.c
demo/common/params.c
demo/config/demo_config.json
demo/config/demo_config_default.json
demo/config/linux_hotspot.json
demo/include/net_abstraction.h
demo/net_esp32c2/esp32c2_backend.c
demo/net_linux/linux_hotspot.c
demo/net_linux/linux_hotspot.h
demo/net_sim/sim_backend.cpp
demo/net_win/win_backend.cpp
demo/net_win/win_backend.h
demo/pc/core/host_provision_client.cpp
demo/tests/test_net_abstraction.cpp
demo/tests/test_params.cpp
Document/beta v1.0.0/TCP_DHCP配网问题修复说明.md
```
