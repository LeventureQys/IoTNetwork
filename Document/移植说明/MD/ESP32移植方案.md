# ESP32 移植方案

> **历史交付件（beta v1.0.6，2026-08-06）**：本文档针对旧版复杂流程与 ESP32-C2 移植，
> beta v1.1 起 ESP32-C2 不在开发与验收范围（仅保留 `demo/device/backends/esp32c2` 与
> `demo/device/firmware/esp32c2` 骨架）。当前流程见仓库根
> [`流程说明.md`](../../../流程说明.md)。本文档内容仅供历史参考。

> 本文档说明 `net_backend_t` 各接口的语义约定与项目特定参数。不包含 ESP-IDF 实现细节，嵌入式开发者拿到接口签名和约定值后自行实现。

## 1. 硬件抽象层概览

设备端在硬件之上定义了一个清晰的抽象层：`net_backend_t` 结构体，包含 29 个函数指针，分为 7 个类别：

| 类别 | 接口数 | 函数列表 |
|------|--------|---------|
| 生命周期 | 2 | `init`、`deinit` |
| WiFi 介质 | 9 | `wifi_scan`、`wifi_sta_connect`、`wifi_sta_disconnect`、`wifi_ap_start`、`wifi_ap_stop`、`wifi_get_rssi`、`wifi_get_ip`、`wifi_get_current_ssid`、`wifi_get_gateway` |
| TCP 非阻塞 | 6 | `tcp_listen`、`tcp_accept`、`tcp_connect`、`sock_send`、`sock_recv`、`sock_close` |
| UDP 组播 | 3 | `udp_mcast_join`、`udp_send`、`udp_recv` |
| mDNS | 3 | `mdns_register`、`mdns_unregister`、`mdns_resolve` |
| NVS | 3 | `nvs_get`、`nvs_set`、`nvs_erase` |
| 系统 | 2 | `time_ms`、`random` |

所有函数第一个参数为 `void *user`（后端私有上下文）。上层代码通过 `net_ctx_t` 封装调用。`src/core/` 下的业务状态机**完全不包含平台特定代码**。

## 2. 生命周期

### init
- **语义**：初始化网络栈，加载配置文件
- **参数**：`config_path` — 配置文件路径（可为 NULL 使用默认配置）
- **返回**：成功 `DEMO_OK`，失败 `DEMO_ERR`

### deinit
- **语义**：清理网络资源，在程序退出前调用

## 3. WiFi 介质接口

### wifi_scan
- **语义**：扫描周边 WiFi 热点
- **输出**：`net_ap_info_t` 数组，每项含 SSID（≤32 字节）、RSSI、`band_2g` 标志（1=2.4GHz）、BSSID（17 字节）
- **约定**：`count` 入参为数组容量，出参为实际数量

### wifi_sta_connect
- **语义**：以 STA 模式连接指定 WiFi
- **输入**：SSID（≤32 字节）、密码（8~63 字节，WPA2 约束）
- **输出**：`wifi_reason_t`

| reason | 含义 | 设备侧处理 |
|--------|------|-----------|
| 0 | 成功 | 进入服务发现 |
| 201 | AP 不可见 | 退避重试（最多 5 次），可切换备用凭据 |
| 202 | 认证失败 | 不可恢复，直接进入配网模式 |
| 205 | 握手超时 | 退避重试 |

- **约定**：连接操作可能是异步的，需同步等待结果后返回

### wifi_sta_disconnect
- **语义**：断开当前 STA 连接

### wifi_ap_start
- **语义**：开启 SoftAP 热点，设备进入配网模式
- **约定值**：
  - SSID：`Modu_XXXX`，`XXXX` = 设备 MAC 后 4 位（大写十六进制）
  - 加密：WPA2-PSK
  - 密码：设备出厂唯一密码（PoP 语义，每台设备不同）
  - PIN：调用 `random` 生成的 4 位随机数字，一次性使用
  - AP IP：`192.168.1.1`
  - 最大连接数：**1**

### wifi_ap_stop
- **语义**：关闭 SoftAP，切回 STA 模式

### wifi_get_rssi
- **语义**：获取当前 WiFi 信号强度
- **输出**：dBm 值（如 -40）
- **约定**：**-75dBm** 为弱信号阈值，设备侧连续 30 秒低于此值触发主动重连

### wifi_get_ip
- **语义**：获取当前 STA 接口 IPv4 地址
- **输出**：uint32_t，**网络字节序**（大端）
- **约定**：上层自行 `inet_ntoa` 等转换为可读格式

### wifi_get_current_ssid
- **语义**：获取当前关联 WiFi 的 SSID
- **输出**：调用方提供缓冲区，容量 ≥33 字节

### wifi_get_gateway
- **语义**：获取当前 STA 接口 IPv4 网关
- **输出**：uint32_t，**网络字节序**

## 4. TCP 接口

全部为非阻塞语义：无数据/无连接时返回 `DEMO_ERR_AGAIN`。

### tcp_listen
- **语义**：在指定端口启动 TCP 服务器
- **约定**：配网端口 **5935**（AP 模式下的 TCP 服务器端口）

### tcp_accept
- **语义**：接受一个客户端连接
- **输出**：`conn` — 客户端 socket，`peer` — 对端地址（网络字节序）

### tcp_connect
- **语义**：以 TCP 客户端连接服务器
- **参数**：`timeout_ms` — 超时毫秒数
- **约定**：连接超时返回 `DEMO_ERR`

### sock_send / sock_recv / sock_close
- **语义**：发送数据、接收数据、关闭 socket
- **帧约定**：所有 TCP 报文使用 **2 字节长度前缀（大端序）+ JSON 负载**，单消息负载上限 **1024 字节**
- **错误处理**：同一连接连续 3 次畸形包（长度非法 / JSON 解析失败 / 未知 cmd）→ 主动断开连接

## 5. UDP 组播接口

### udp_mcast_join
- **语义**：创建 UDP socket 并加入组播组
- **约定值**：组播地址 **224.0.2.1**，端口 **5936**，TTL=1

### udp_send
- **语义**：向组播地址发送数据包
- **约定**：UDP 报文不加长度前缀，直接发送 JSON

### udp_recv
- **语义**：接收组播数据包，无数据返回 `DEMO_ERR_AGAIN`

## 6. mDNS 接口

### mdns_register
- **语义**：注册 mDNS 服务
- **约定值**：服务类型 `_tactile._tcp`，实例名 `host`，携带 IP 和 TCP 端口的 TXT 记录

### mdns_unregister
- **语义**：注销 mDNS 服务

### mdns_resolve
- **语义**：解析 `host._tactile._tcp.local`
- **参数**：`timeout_ms` — 超时毫秒数
- **约定**：超时返回 `DEMO_ERR`

## 7. NVS 接口

### nvs_get / nvs_set / nvs_erase
- **语义**：读写/擦除持久化键值配置
- **约定值**：命名空间固定为 **`"provision"`**

| 键名 | 内容 | 长度 |
|------|------|------|
| `wifi_creds` | WiFi 凭据集（最多 2 组，含 SSID、密码） | JSON 字节流 |
| `host_candidates` | 主机候选列表（最多 2 条，含 IP、port、last_seen） | JSON 字节流 |
| `evlog` | 事件日志环形缓冲（最多 50 条） | JSON 字节流 |

- **约定**：键不存在时，`nvs_get` 返回 `DEMO_ERR` 且 `*len=0`

## 8. 系统接口

### time_ms
- **语义**：返回系统毫秒级时间戳
- **约定**：用于心跳定时、超时计算、上电错峰延迟

### random
- **语义**：返回随机数（uint32_t）
- **约定**：用于生成 PIN 码（4 位数字）、计算上电错峰延迟（0~10 秒）

## 9. 关键约定值速查表

| 参数 | 值 | 说明 |
|------|----|------|
| TCP 配网端口 | 5935 | AP 模式设备 TCP 服务器端口 |
| TCP 业务端口 | 5935 | 上位机 TCP 服务器端口 |
| UDP 组播地址 | 224.0.2.1 | 兜底服务发现 |
| UDP 组播端口 | 5936 | — |
| mDNS 服务类型 | `_tactile._tcp` | 实例名 `host` |
| AP SSID 前缀 | `Modu_` | 后缀为 MAC 后 4 位大写十六进制 |
| AP 网关 IP | 192.168.1.1 | — |
| AP WPA2 密码 | 每设备唯一 | 出厂标签，PoP 语义 |
| PIN 码 | 4 位随机数字 | 一次性，认证成功后作废 |
| AP 最大连接数 | 1 | — |
| WiFi 凭据集容量 | 2 组 | 主/备，NVS 键 `wifi_creds` |
| 主机候选列表容量 | 2 条 | 主/备，NVS 键 `host_candidates` |
| NVS 命名空间 | `"provision"` | — |
| 事件日志容量 | 50 条 | NVS 键 `evlog` |
| 消息帧头长度 | 2 字节 | 大端序长度前缀 |
| 消息负载上限 | 1024 字节 | — |
| 协议版本 proto_ver | 1 | 握手协商依据 |
| 上电错峰范围 | 0~10 秒 | 随机延迟，防多设备同步风暴 |
| 心跳间隔 | 10 秒 | — |
| 心跳判死倍数 | 1.5x | 15 秒无报文判死 |
| 配网认证超时 | 10 秒 | auth 必须在连接后此时间内完成 |
| 配网信息等待超时 | 60 秒 | 认证通过后等待 wifi_config |
| 热点无连接超时 | 120 秒 | — |
| 热点关闭退避 | 5 分钟 | — |
| PIN 错误上限 | 5 次 | — |
| WiFi 重试上限 | 5 次 | 可恢复错误重试次数 |
| 重连退避 | 1s ~ 30s | 指数退避 + 0~5s 抖动 |
| busy 长退避 | 60 秒 | 上位机连接满后重试间隔 |
| RSSI 弱信号阈值 | -75 dBm | — |
| RSSI 采样间隔 | 5 秒 | — |
| RSSI 劣化判定 | 30 秒 | 连续低于阈值时长 |
| 快速发现窗口 | 30 秒 | 配网/收到 host_bye 后快速组播轮询 |
| 快速发现间隔 | 500 毫秒 | — |
| 上位机连接上限 | 16 | — |
| 配网确认窗口 | 120 秒 | 新配置生效后未连上上位机则回滚 |
| 连续畸形包断连 | 3 次 | — |

## 10. 项目架构注意事项

1. **上层零平台依赖**：`src/core/device_app.c` 及所有业务代码通过 `net_ctx_t` 访问硬件，不含任何平台特定 include 或 API 调用。移植只需实现 `net_backend_t`。

2. **真实环境无虚拟 IP**：设备在真实硬件上通过 mDNS/组播获取的是上位机的**真实 IP**，`tcp_connect` 直连即可。模拟后端中的虚拟 IP 翻译逻辑在真实环境下不存在。

3. **消息帧格式**：TCP 报文统一使用 2 字节长度前缀（大端序）+ JSON 负载。UDP 报文不加前缀（天然数据报成帧）。发送方负责组帧，接收方负责解帧与校验。

4. **配网先连后存**：收到 wifi_config 后，先尝试连接目标 WiFi，连接成功才将凭据写入 NVS。连接失败则原配置不变，防止将无效凭据持久化。

5. **PIN 一次性**：认证成功后立即作废，下一次配网需重新生成。连续 5 次错误 → 关闭热点 5 分钟。

6. **多凭据 fallback**：NVS 最多保存 2 组 WiFi 凭据（主/备）。主凭据连续 5 次失败 → 自动切换备用。全部耗尽 → 进入配网模式。
