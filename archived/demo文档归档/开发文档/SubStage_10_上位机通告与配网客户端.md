# SubStage_10：上位机通告/发现与配网客户端（host_announcer + host_mdns + host_provision_client）

## SubStage: 上位机通告与配网客户端
- 所属 Stage：Stage 4
- 依赖前置：SubStage_09（host_registry / host_tcp_server 头文件）
- 并行状态：需等待 SubStage_09 头文件；可与 SubStage_11 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

实现上位机剩余模块：UDP 组播通告器（host_announce 周期广播 + 退出前 host_bye）、mDNS 服务注册（sim 注册表）、配网客户端（扫描虚拟热点 → 连接 → auth → wifi_config → wifi_result → close_ap 串行流程，含 PIN 自动获取与频段过滤）。C++17。

## 2. 当前代码状态

- 已有：SubStage_01~03、09（本任务书依赖 09 的头文件签名，09 的 .cpp 未完成也可先行开发头文件调用）。
- 依据：`设计文档.md` 4.15（契约）；`协议文档.md` 4.1/4.2（C1~C7）、5.1/5.2（配网与发现时序）、6.2（频段约束）；`流程说明.md` 3.2.2（上位机配网侧 17~22）、3.3（23~27）。

## 3. 交付物（全部新建，C++17）

```
demo/host/host_announcer.h
demo/host/host_announcer.cpp
demo/host/host_mdns.h
demo/host/host_mdns.cpp
demo/host/host_provision_client.h
demo/host/host_provision_client.cpp
```

## 4. 接口契约

```cpp
// host_announcer.h
class HostAnnouncer {
public:
    HostAnnouncer(net_ctx_t* net, const demo_params_t& params);
    ~HostAnnouncer();
    int  Start();                       // udp_mcast_join(mcast_group, mcast_port)
    void Stop();                        // 关闭组播 socket
    void Poll(uint64_t now_ms);         // 每 1s（params 可配置）发一次 host_announce，seq 递增
    void SendBye();                     // 退出前广播 host_bye（seq 继续递增）
    uint64_t last_seq() const;
};

// host_mdns.h
class HostMdns {
public:
    HostMdns(net_ctx_t* net, const demo_params_t& params);
    void Register();                    // mdns_register：instance="host", type="_tactile._tcp",
                                        //   addr=(虚拟 IP, host_tcp_port), txt="ip=...;tcp_port=..."
    void Unregister();                  // mdns_unregister("_tactile._tcp")
};

// host_provision_client.h
class HostProvisionClient {
public:
    HostProvisionClient(net_ctx_t* net, const demo_params_t& params);
    /* 扫描并配网一台设备：返回 0=ok，-1=失败（日志含失败点） */
    int ProvisionDevice(const char* ap_ssid);
    int ScanAps(std::vector<std::string>* modu_ssids);   // 过滤 "Modu_" 前缀且 band_2g
};
```

## 5. 实现细节

### 5.1 HostAnnouncer

- `Start`：`net_udp_mcast_join(mcast_group, mcast_port, &sock_)`；`seq_ = 1`。
- `Poll`：距上次发送 ≥ `announce_interval_ms`（默认 1000，参数表未单独定义 → 用 `discovery_normal_interval_ms`）→ 构造 `host_announce{proto:PROTO_VERSION, seq:seq_++, ip:params->host_virtual_ip, tcp_port:params->host_tcp_port}` → `net_udp_send(mcast_group, mcast_port, ...)`（UDP 无帧前缀，直接 JSON 文本）。
- `SendBye`：构造 `host_bye{seq:seq_++}` → send → 日志 `host_bye sent`。
- 退出时 Stop 关闭 socket。

### 5.2 HostMdns

- `Register`：`net_mdns_register`，`svc.instance="host"`、`svc.type=PROTO_MDNS_TYPE`、`svc.addr.ip = inet 解析 host_virtual_ip`、`svc.addr.port = host_tcp_port`、`svc.txt = "ip=<virtual_ip>;tcp_port=<port>"`。
- 日志 `mdns registered: host._tactile._tcp.local`.

### 5.3 HostProvisionClient::ProvisionDevice（阻塞式，总超时 = 参数表中各超时之和 ×2）

```text
1) ScanAps：net_wifi_scan → 过滤 ssid 以 "Modu_" 开头 且 band_2g==1 → 列表；
   找不到目标 ssid → 返回 -1（日志）。
2) 连接虚拟热点：net_wifi_sta_connect(ap_ssid, ap_password, &reason)：
   - ap_password 从哪来？→ sim 后端 ApFind 反查（模拟"设备标签密码"）。
   实现：调用 net_inject 不可行（无该语义）——由 sim 后端提供专用查询？
   决议：sim 后端 `wifi_scan` 返回的 net_ap_info 仅含 ssid/rssi/band/bssid；
   增加语义：**sim 后端的 ApFind 通过 `net_inject("get_ap_info", "{\"ssid\":...}")`
   返回密码/PIN（JSON 字符串，注入通道双向复用）**。SubStage_03 实现此约定。
   失败（reason!=OK）→ 日志 + 返回 -1。
3) net_tcp_connect(DeviceApVirtualIp:5935 即 192.168.1.1:5935, 3000ms)（sim 翻译到真实端口）。
4) auth：从注入通道获取 PIN → 发 `auth{pin}` → 等 `auth_result`（provision_auth_timeout_ms×2）：
   - ok → 继续；fail → 日志 + 返回 -1。
5) 发 `wifi_config{ssid:params->target_ssid, password:params->target_password}` →
   等 `wifi_result`（provision_wifi_cfg_timeout_ms×2）：
   - ok → 日志（含 device_ip）→ 发 `close_ap` → 断开配网连接 → `net_wifi_sta_disconnect`（切回目标 WiFi，模拟）→ 返回 0。
   - fail → 日志（含 reason）→ 断开连接 → 返回 -1（可重试）。
6) 任意等待超时/连接关闭 → 日志 + 返回 -1。
```

- 收包使用帧解析（frame_parse 累积缓冲），复用 cJSON。
- 所有日志模块前缀 `[HOST]`，配网步骤逐条打印（验收证据）。

## 6. 验收标准（tests/ 增加 test_announcer.cpp 与 test_provision_client.cpp）

1. Announcer：Start 后 Poll 发出 announce（device 侧组播可收到，字段校验 proto/seq/ip/tcp_port）；seq 连续递增；SendBye 发出 bye。
2. Mdns：Register 后 device 侧 mdns_resolve 返回正确地址（进程内注册表）。
3. ProvisionClient 全流程（模拟 device AP 已开）：ScanAps 过滤正确（含 5G 虚拟 AP 应被过滤——sim 注入 band_2g=0 的 AP 验证）；配网成功 → 日志断言各消息序；目标网络 auth_fail 注入 → wifi_result fail 路径返回 -1。
4. PIN 错误场景：device 端 PIN 被消耗/错误 → auth_result fail → -1。
5. 无目标 AP → -1（scan 空）。
6. close_ap 后 device 迁移 DISCOVERY（与 device 集成测试联动）。

## 7. 禁止事项

- 不实现 HostApp 主循环（SubStage_11）。
- 不直接调用 Winsock。
- 不修改抽象层契约（注入通道 get_ap_info 为 sim 后端扩展，SubStage_03 已预留 inject 通道；本模块只消费）。
- 不轮询组播（Announcer 是发送方；接收在 device 侧）。

## 8. 依赖前置

- 等待 SubStage_09 头文件（其实本模块不依赖 09——修正：本模块仅依赖 SubStage_02/03 与 params；并行状态改为**与 SubStage_09 并行，无需等待**）。
- 集成时加入 CMake provision_demo_core。
