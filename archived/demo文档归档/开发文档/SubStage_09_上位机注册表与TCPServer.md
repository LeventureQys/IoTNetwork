# SubStage_09：上位机注册表与 TCP 服务器（host_registry + host_tcp_server）

## SubStage: 上位机注册表与 TCP 服务器
- 所属 Stage：Stage 4
- 依赖前置：SubStage_02、SubStage_03（抽象层 + sim 后端）
- 并行状态：需等待 SubStage_02/03；可与 SubStage_10/11 并行（本任务书先完成，10/11 依赖其头文件）
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

实现上位机核心：设备注册表（ID→连接映射、重复连接清理、16 上限 busy、心跳丢包统计、离线判定、质量统计）与 TCP 服务器（poll 非阻塞多连接、长度前缀帧解析、device_hello → host_ack 全语义）。C++17。

## 2. 当前代码状态

- 已有：SubStage_01~05（含 device_priv.h 等设备侧；host 与其无依赖）。
- 依据：`设计文档.md` 4.15（契约）；`协议文档.md` 4.3（C8~C11）、5.3（注册时序）、6.1/6.3（会话与版本规则）；`流程说明.md` 3.4（29~31）、3.5.3（39~41）。

## 3. 交付物（全部新建，C++17）

```
demo/host/host_registry.h
demo/host/host_registry.cpp
demo/host/host_tcp_server.h
demo/host/host_tcp_server.cpp
```

## 4. 接口契约

```cpp
// host_registry.h
struct DeviceEntry {
    std::string id, fw_version, session_id, state;   // state: "online"/"offline"
    int proto_ver = 1;
    uint64_t last_rx_ms = 0, first_seen_ms = 0, last_seen_ms = 0;
    uint32_t last_ping_seq = 0;
    uint32_t lost_ping_count = 0;
    int reconnect_count = 0;
    int last_rssi = 0;
    void *conn = nullptr;
};
class HostRegistry {
public:
    DeviceEntry* Find(const std::string& id);
    DeviceEntry* Add(const std::string& id, void* conn);   // 重复 id：关闭旧 conn 并替换（清理旧连接）
    void Remove(const std::string& id);
    void RemoveAll();
    size_t Size() const;
    void OnRx(const std::string& id);                      // 刷新 last_rx/last_seen
    void OnPing(const std::string& id, uint32_t seq);      // seq 不连续 → lost_ping_count += 缺口
    void MarkOffline(const std::string& id);               // 心跳超时：state=offline（保留条目）
    void DeleteOffline(const std::string& id);             // 彻底删除（连接关闭后）
    std::vector<std::string> FindDead(uint64_t now_ms, int dead_ms) const; // 返回超时 online id
};

// host_tcp_server.h
class HostTcpServer {
public:
    HostTcpServer(net_ctx_t* net, HostRegistry& reg, const demo_params_t& params);
    ~HostTcpServer();
    int  Start();                                          // tcp_listen(host_tcp_port)
    void Poll(uint64_t now_ms);                            // accept + 各连接收包分发 + 心跳监控
    void BroadcastCloseAll();                              // 退出前关闭所有连接
    void SetBusyOverride(int limit);                       // busy_inject：临时连接上限（<0 恢复）
    const demo_params_t& params() const;
};
```

## 5. 实现细节

### 5.1 HostTcpServer::Poll（每轮调用）

1. `net_tcp_accept(listen)`：新连接 → 如果当前连接数（registry.Size()）≥ 上限（`busy_override >= 0 ? busy_override : host_max_conn`）→ **立即回复 busy 帧**（`host_ack{busy}`，frame_wrap 后 send）→ 关闭该连接 + 拒绝日志；否则 → 挂到"待注册连接"集合（`pending_conns`，等待 device_hello，5s 未 hello 则关闭——`hello_timeout_ms`）。
2. 每个 pending 连接：recv → frame_parse 累积 → 完整帧 → 解析 cmd：
   - `device_hello` → 校验字段（id/type/fw_version/proto_ver/capabilities/uptime 必选；session_id 可选）→ 协议版本协商：`device proto_ver != PROTO_VERSION` → 回复 `host_ack{fail,"reason":"协议版本过低"}` + 关闭 + 日志（待升级清单）；否则 → 注册处理（见 5.3）→ 从 pending 移入 registry（entry.conn = sock）。
   - 其他 cmd（未知命令）→ 按协议文档 2.6 前向兼容规则：notification 型仅日志（不计畸形）；request 型回复通用占位应答 `{"cmd":"<name>","data":{"status":"unsupported"}}`；字段错误/帧错误才计入畸形（pending 阶段畸形 ≥ 3 → 关闭）。
3. 每个 registry 中 online 连接：recv → frame_parse → 解析：
   - `ping` → `reg.OnRx(id)` + `reg.OnPing(id, seq)` + 回复 `pong`；`seq <= last`（乱序）→ 忽略（不回复）。
   - 未知命令 → 按协议文档 2.6 前向兼容规则：notification 型仅日志（不计畸形）；request 型回复通用占位应答 `{"cmd":"<name>","data":{"status":"unsupported"}}`；`app_data` → 日志后丢弃（通用占位，不崩溃）；字段/帧错误才计入畸形（≥3 关闭）。
4. 心跳监控：`reg.FindDead(now, heartbeat_dead_ms)` → 每个超时 id：`MarkOffline` + 关闭连接（`net_sock_close(entry.conn)`，entry.conn=nullptr）+ 日志 `device offline (heartbeat timeout)`。offline 条目保留（供重连续接）。
5. 连接关闭检测：recv 返回 DEMO_ERR（对端关闭）→ 若 entry 存在且 online → `MarkOffline` + 日志 `device disconnected`；pending 连接 → 移除。

### 5.2 帧解析与回复辅助（host_tcp_server.cpp 内部）

- 每连接接收缓冲：`std::vector<uint8_t>`（host 侧可用 STL）追加 recv 数据 → frame_parse 循环取帧。
- 发送辅助：`send_frame(void* sock, cJSON* obj)`：cJSON_PrintUnformatted → frame_wrap → net_sock_send。
- 时间戳：`now_ms` 由 Poll 入参传入（HostApp 统一提供 net_time_ms）。

### 5.3 注册处理（device_hello → host_ack）

```text
1) 查 reg.Find(id)：
   - 存在且 conn != nullptr（旧连接仍在线）→ 关闭旧 conn（日志 "duplicate connection, old closed"）；
   - 存在（offline）→ 复用条目：reconnect_count++；若 hello 携带 session_id 且等于条目 session_id → 日志 "session resumed"（续接）；
   - 不存在 → Add（first_seen 记录）。
2) 生成 host_ack{ok}：heartbeat_interval = params->heartbeat_interval_ms/1000（秒）、
   session_id = 8 字节随机十六进制（新注册生成；续接时保留原值）、
   server_time = time(nullptr)、proto_ver = PROTO_VERSION、fw_min_req = "1.0.0"。
3) 保存条目 session_id / fw_version / proto_ver → 发送 → 日志 "device registered: <id> session=<sid>"。
```

### 5.4 HostRegistry 细节

- `Add` 重复 id：返回新条目前关闭旧 conn（旧 conn 句柄由调用方提供的回调执行关闭——实现：Add 内部不关闭 socket（不知道句柄类型），改为：**HostTcpServer 在 Add 前自行检查 Find 并关闭旧 conn**，Add 只负责条目替换。契约按此执行）。
- `OnPing`：`seq - last_ping_seq > 1` → `lost_ping_count += (seq - last_ping_seq - 1)`；`seq <= last_ping_seq` → 忽略；更新 last_ping_seq。
- `MarkOffline`：state="offline"；last_seen 更新。
- `FindDead`：遍历 online 条目，`now - last_rx_ms > dead_ms` 收集。

## 6. 验收标准（tests/ 增加 test_registry.cpp 与 test_tcp_server.cpp）

1. 注册表：Add/Find/Remove；重复 id 替换（旧 conn 句柄被关闭——用伪句柄计数验证）；OnPing 丢包统计（seq 1,2,5 → lost=2）；FindDead 超时判定。
2. TCP 服务器（sim 后端真实 socket）：模拟 device 客户端完整握手——connect → hello → 收 host_ack ok（字段齐全，session_id 8 位十六进制）→ ping → 收 pong → 心跳判死（压缩 1s）→ offline。
3. 重复注册：同 id 二次 hello（先断开再连）→ 旧连接关闭日志 + 续接成功（session_id 一致）。
4. busy：上限设 1（SetBusyOverride(1)）→ 第二个客户端收到 host_ack busy。
5. 协议不兼容：hello proto_ver=2 → host_ack fail + 关闭。
6. 畸形 3 次 → 连接被关闭。
7. 对端断开（device 关闭 socket）→ MarkOffline 日志。

## 7. 禁止事项

- 不实现组播通告/mDNS/配网客户端（SubStage_10）。
- 不实现 HostApp 主循环（SubStage_11）。
- 不修改 net_abstraction.h / device 侧代码。
- 不直接调用 Winsock API（全部经抽象层）。

## 8. 依赖前置

- 等待 SubStage_02/03；与 SubStage_10/11 并行（本任务书产出头文件供 10/11 使用）。
- 需在根 CMakeLists.txt 的 provision_demo_core 中加入 host/*.cpp（MainAgent 集成）。
