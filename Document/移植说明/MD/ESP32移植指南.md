# ESP32 移植指南

> 本文档为嵌入式工程师提供 ESP32 后端移植的任务清单。所有任务均围绕本项目的硬件抽象层（`net_backend_t`，29 个接口）展开。

## 1. 理解抽象层

- [ ] 阅读 `net_abstraction.h`：29 个接口的签名与语义，位于 `demo/device/include/net_abstraction.h`
- [ ] 阅读 ESP32-C2 骨架代码：`demo/device/backends/esp32c2/esp32c2_backend.c`，每个接口已有占位实现和注释
- [ ] 理解 `net_ctx_t` 封装层：上层代码通过它调用，不直接依赖具体硬件。`src/core/` 下所有业务逻辑**零平台依赖**

## 2. 接口实现清单

### 2.1 生命周期（2 个）

- [ ] `init`：初始化网络栈，加载配置文件
- [ ] `deinit`：清理网络资源

### 2.2 WiFi 介质（9 个）

- [ ] `wifi_scan`：扫描周边 WiFi，填充 `net_ap_info_t` 数组（SSID ≤32 字节、RSSI、频段、BSSID）
- [ ] `wifi_sta_connect`：STA 模式连接指定 WiFi，返回连接结果与 `wifi_reason_t`（reason code 对齐：201/202/205）
- [ ] `wifi_sta_disconnect`：断开当前 STA 连接
- [ ] `wifi_ap_start`：开启 SoftAP 热点
  - SSID 格式：`Modu_XXXX`（XXXX = MAC 后 4 位大写十六进制）
  - 加密：WPA2-PSK，密码为设备出厂唯一密码
  - PIN：4 位随机数字，通过日志/串口输出
  - AP IP：192.168.1.1
  - 最大连接数：1
- [ ] `wifi_ap_stop`：关闭 SoftAP
- [ ] `wifi_get_rssi`：获取当前 STA 信号强度（dBm），-75dBm 为弱信号阈值
- [ ] `wifi_get_ip`：获取当前 STA 接口 IPv4 地址（网络字节序）
- [ ] `wifi_get_current_ssid`：获取已关联 WiFi 的 SSID（调用方提供 ≥33 字节缓冲区）
- [ ] `wifi_get_gateway`：获取当前 STA 接口网关地址（网络字节序）

### 2.3 TCP（6 个）

- [ ] `tcp_listen`：在指定端口启动 TCP 服务器，监听端口：**5935**
- [ ] `tcp_accept`：接受客户端连接，无连接返回 `DEMO_ERR_AGAIN`，输出对端地址（网络字节序）
- [ ] `tcp_connect`：连接服务器，timeout_ms 内未成功返回 `DEMO_ERR`
- [ ] `sock_send`：发送数据。帧格式：**2 字节长度前缀（大端序）+ JSON 负载**，单消息上限 **1024 字节**
- [ ] `sock_recv`：接收数据，无数据返回 `DEMO_ERR_AGAIN`
- [ ] `sock_close`：关闭 socket

### 2.4 UDP 组播（3 个）

- [ ] `udp_mcast_join`：创建 UDP socket 并加入组播组 **224.0.2.1:5936**（TTL=1）
- [ ] `udp_send`：向组播地址发送数据
- [ ] `udp_recv`：接收组播数据，无数据返回 `DEMO_ERR_AGAIN`

### 2.5 mDNS（3 个）

- [ ] `mdns_register`：注册服务 `host._tactile._tcp.local`，含 IP 和 TCP 端口的 TXT 记录
- [ ] `mdns_unregister`：注销服务
- [ ] `mdns_resolve`：解析 `host._tactile._tcp.local`，timeout_ms 内未解析返回 `DEMO_ERR`

### 2.6 NVS（3 个）

- [ ] `nvs_get`：读取键值。命名空间：**`"provision"`**

| 键名 | 内容 | 格式 |
|------|------|------|
| `wifi_creds` | WiFi 凭据集，最多 2 组（主/备） | JSON 字节流 |
| `host_candidates` | 主机候选列表，最多 2 条（IP+port + last_seen） | JSON 字节流 |
| `evlog` | 事件日志环形缓冲，最多 50 条 | JSON 字节流 |

- [ ] `nvs_set`：写入键值，键名同上
- [ ] `nvs_erase`：删除指定键。键不存在返回 `DEMO_ERR` 且 `*len=0`

### 2.7 系统（2 个）

- [ ] `time_ms`：返回毫秒级时间戳
- [ ] `random`：返回随机数（用于 PIN 生成和上电错峰延迟）

## 3. 流程功能验证

### 3.1 启动与自检

- [ ] 新设备（NVS 无 WiFi 配置）上电 → 自动进入配网模式（AP 模式）
- [ ] 已配网设备上电 → 读取 NVS 配置 → 尝试连接已保存 WiFi → 成功则进入服务发现
- [ ] 上电错峰：随机延迟 0~10 秒后开始连接
- [ ] 连接失败分类：认证失败（不可恢复，进配网模式）、信号弱（退避重试，最多 5 次）
- [ ] 多凭据 fallback：主凭据 5 次失败 → 自动切换备用凭据（如有）

### 3.2 SoftAP 配网

- [ ] 设备开启热点 `Modu_XXXX`，上位机可扫描到
- [ ] 上位机连接热点（WPA2 密码 + PIN），通过 TCP 5935 认证
- [ ] 认证通过后接收 `wifi_config` → 先尝试连接目标 WiFi → 成功才写 NVS（先连后存）
- [ ] 配网确认成功后回复 `wifi_result: ok` → 收到 `close_ap` → 关闭热点 → 进入服务发现
- [ ] PIN 认证成功后即作废（一次性）；连续 5 次错误 → 关闭热点
- [ ] AP 开启 120 秒无认证 → 关闭热点，退避 5 分钟

### 3.3 服务发现

- [ ] mDNS 解析 `host._tactile._tcp.local` → 获取上位机 IP 和端口
- [ ] mDNS 失败 → UDP 组播监听 224.0.2.1:5936 → 接收 `host_announce`
- [ ] 组播也失败 → 尝试 NVS 中保存的候选列表（主/备地址）
- [ ] 全部失败 → 循环重试（退避上限 30 秒）

### 3.4 连接与心跳

- [ ] 设备 TCP 连接上位机 → 发送 `device_hello`（含 MAC、fw_version、proto_ver）
- [ ] 收到 `host_ack` → 获取心跳间隔、session_id、server_time
- [ ] 每 10 秒发送 `ping`（带 seq 序号），回复 `pong`
- [ ] 15 秒（1.5x 心跳间隔）无任何报文 → 判定连接失效

### 3.5 异常自愈

- [ ] TCP 断线：退避重连（1s→30s），30 秒未成功 → 回到服务发现
- [ ] WiFi 断开：退避重连，5 次失败 → 回到启动判断
- [ ] 收到 `host_bye`：立即转候选列表直连 + 快速轮询
- [ ] 收到 `busy` 响应：60 秒长退避后重试
- [ ] RSSI 持续低于 -75dBm（30 秒）→ 主动重连
