# SubStage SS2.3：Linux 设备业务状态机与 UI

## SubStage: Linux 设备业务状态机与 UI
- 所属 Stage：Stage 2 - 端侧实现
- 依赖前置：SS1.1
- 并行状态：可与 PC 端 SS2.1/SS2.2 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

设备只在 Linux 真实后端和 sim 后端运行。启动后扫描配置的 PC 热点，用固定密码连接，然后直连 `192.168.137.1:5935`。删除设备热点、凭据下发、服务发现和多凭据业务。

## 2. 当前代码

- 七态枚举：`demo/device/include/protocol.h:47`（SS1.1 将先修改）。
- 大量凭据/AP/discovery字段：`demo/device/src/core/device_priv.h:23`。
- 旧状态机：`demo/device/src/core/device_app.c:655` 至 `demo/device/src/core/device_app.c:945`。
- 配网模块：`demo/device/src/core/device_provision.c:26`。
- 发现模块：`demo/device/src/core/device_discovery.c:73`。
- 会话模块可复用：`demo/device/src/core/device_session.c:11`。
- Linux scan/STA/TCP 已实现：`demo/device/backends/linux/linux_wifi.c:296`、`demo/device/backends/linux/linux_wifi.c:76`、`demo/device/backends/linux/linux_socket.c:85`。

## 3. 文件所有权

允许修改设备 core、runtime 适配、UI、顶层 CMake、设备测试、README。允许从 `device_core` 源列表移除 provision/discovery；文件可保留但不得编译引用。允许按需删除完全无引用的旧业务文件，须由 MainAgent确认。

禁止修改 `backends/linux/linux_hotspot*`、ESP32、PC 和 integration runner。Linux WiFi/socket 只允许修复接口契约明确暴露的局部错误，并须单测。

## 4. 状态机

严格实现设计文档第 9 节六态：BOOT→WIFI_SCAN→STA_JOIN→CONNECT→SESSION→HEAL。

扫描最多 32 条，精确比较 `pc_ap_ssid` 且要求 2.4GHz。无目标按 WiFi退避重试。STA 成功必须验证 IP非0和实际SSID。固定地址通过安全 IPv4 parser 写入 `sess_host`，禁止从 gateway/discovery 推导。

HEAL：WiFi健康→TCP退避回CONNECT；WiFi无效→disconnect后回WIFI_SCAN。busy使用 `busy_backoff_ms`。

## 5. 内部结构

删除 credentials/AP/discovery/candidate字段；保留单一 sess_host、sess_sock、session_id、heartbeat、退避、RSSI和事件日志。`device_app_handle_rx` 改为只处理 session。

`session_connect` hello 字段按设计 10.3；ack 不再读取 server_time/fw_min_req；pong seq校验/事件保留。

## 6. Linux 后端调用边界

允许调用：wifi_scan、wifi_sta_connect/disconnect、wifi_get_rssi/ip/current_ssid、tcp_connect、sock_send/recv/close、NVS事件日志、time/random。

必须修复当前创建期耦合：`device_linux_backend.c:431` 不再无条件创建需要 root 的 hotspot；`linux_wifi_create` 改为直接接收 `const char *sta_interface`，不依赖 `linux_hotspot_t`。纯 STA Linux backend 在普通用户下只要 nmcli/网卡权限满足就可创建。vtable 的AP start/stop明确返回错误。

不得调用：wifi_ap_start/stop、tcp_listen/accept、UDP、mDNS、凭据 NVS。保留 `linux_hotspot*` 实现和独立测试，不重写hostapd/dnsmasq。

## 7. UI

六节点：启动、扫描PC热点、连接热点、连接PC、会话在线、异常重连。删除设备 AP SSID/密码/PIN/配网端口显示。展示目标SSID、PC固定地址、当前SSID/IP/RSSI、会话状态。

## 8. 事件

至少实现设计文档 11节设备事件。所有失败事件含 reason/code，不含密码。

## 9. 测试

使用 fake backend 覆盖：

1. 配置非法启动失败。
2. 扫描无目标持续退避。
3. 扫描有相似但非精确SSID不连接。
4. 精确目标→STA成功→IP/SSID验证→固定TCP地址。
5. auth fail 202→HEAL→重试，不启动AP。
6. DHCP/IP为0→disconnect→scan。
7. TCP失败但WiFi健康→CONNECT重试。
8. WiFi失效→WIFI_SCAN。
9. hello/ack正常、busy长退避、心跳超时。
10. app_data边界。
11. 后端调用计数断言旧 AP/mDNS/UDP/listen 从未调用。
12. QtTest 六节点和无旧文案。

Linux 后端既有 fake ops 测试必须继续通过。

## 10. 启动测试

- sim offscreen 单进程可等待 PC catalog，不崩溃。
- 与 PC sim 双进程由 SS3.1 验证完整路径。
- Linux 真实后端需在具备 nmcli/无线网卡环境运行核心路径；环境缺失报告 MainAgent，不能宣称通过。

## 11. 验收标准

- CMake 不再编译 device_provision/device_discovery。
- 状态和UI只有六态。
- 运行日志无设备热点/配网/发现主路径。
- fake backend证明固定IP和旧能力未调用。
- 单元测试与启动测试通过或真实环境明确阻塞。

## 12. 完成报告

列出修改文件、状态转换、Linux真实测试状态、命令和证据。