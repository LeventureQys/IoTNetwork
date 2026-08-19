# SubStage_03：sim 后端（网络抽象层模拟实现）

## SubStage: sim 后端实现
- 所属 Stage：Stage 2
- 依赖前置：SubStage_02（net_abstraction.h 定稿）
- 并行状态：需等待 SubStage_02；可与 SubStage_04 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

实现网络抽象层的 sim 后端（C++17，Winsock2），运行于宿主机：真实 socket 全链路（TCP loopback、UDP 组播）+ 模拟 WiFi 介质（SimWorld 虚拟热点/目标网络/STA 连接状态机）+ 进程内 mDNS 注册表 + 文件 NVS + 时间/随机 + 故障注入接口。多个实例（host ×1、device ×N）各自持有一个后端 user 数据，共享 SimWorld（互斥锁保护）。

## 2. 当前代码状态

- 已有：SubStage_01（CMake、common）、SubStage_02（net_abstraction.h）。
- 依据：`设计文档.md` 2.3（虚拟网络语义）、2.2（模块架构）、4.6（接口契约）、阶段二问题清单 Q-D1/Q-D2/Q-D4/Q-D9/Q-D16/Q-D17。

## 3. 交付物（全部新建）

```
demo/net_sim/sim_backend.h/.cpp     # 实例上下文 + vtable 适配 + 线程归属
demo/net_sim/sim_world.h/.cpp       # SimWorld：虚拟 AP 表 / 目标网络 / mDNS 注册表（互斥锁）
demo/net_sim/sim_socket.cpp         # TCP/UDP/组播 真实 Winsock 封装
demo/net_sim/sim_wifi.cpp           # 虚拟 WiFi 介质（scan/STA/AP/RSSI/虚拟 IP）
demo/net_sim/sim_mdns.cpp           # 进程内 mDNS 注册表
demo/net_sim/sim_nvs.cpp            # 文件 NVS（nvs_dev<n>.json）
demo/net_sim/sim_inject.h/.cpp      # 剧本注入接口（main 线程调用）
```

## 4. 接口契约

### 4.1 工厂（sim_backend.h，C++，对 C 侧暴露 C 函数）

```cpp
extern "C" {
    /* 创建 sim 后端实例：tag = "host" 或 "dev<n>"（n=设备索引，0 起）；params 为全局配置 */
    void *sim_backend_create(const char *tag, const demo_params_t *params);
    void  sim_backend_destroy(void *user);
    /* 实例的后端表（单例静态表，所有实例共享同一 vtable） */
    const net_backend_t *sim_backend_table(void);
}
```

### 4.2 SimWorld（sim_world.h，进程级单例，全部方法互斥锁保护）

```cpp
struct SimAp { char ssid[33]; char password[64]; char pin[8]; uint16_t real_port;
               char owner_tag[16]; bool active; bool pin_consumed; };
struct SimNetwork { char ssid[33]; char password[64]; bool band_2g; bool up; bool auth_fail_injected; };
struct SimMdnsSvc { char instance[64]; char type[64]; uint32_t ip; uint16_t port; char txt[128]; bool active; };

class SimWorld {
public:
    static SimWorld& Instance();
    // AP（device 侧调用）
    void ApRegister(const char *owner_tag, const char *ssid, const char *pass, const char *pin, uint16_t real_port);
    void ApUnregister(const char *owner_tag);
    bool ApFind(const char *ssid, SimAp *out);                 // scan/连接用
    bool ApGetPin(const char *ssid, char *pin_out, int cap);   // host 模拟"读设备标签"
    // 目标网络（配置注册一次）
    void TargetNetworkSet(const char *ssid, const char *pass, bool band_2g);
    // STA 连接判定（返回 reason）
    int  StaConnect(const char *ssid, const char *pass);       // 0=ok 否则 wifi_reason_t
    void TargetSetUp(bool up);                                 // wifi_disconnect / wifi_ok 注入
    void TargetSetAuthFail(bool on);                           // wifi_auth_fail 注入
    // RSSI（按设备实例 tag 存储，默认 -58）
    void RssiSet(const char *tag, int rssi);                   // rssi_set 注入
    int  RssiGet(const char *tag);
    // mDNS 注册表
    void MdnsRegister(const SimMdnsSvc &svc);
    void MdnsUnregister(const char *type);
    bool MdnsResolve(const char *type, SimMdnsSvc *out);
    // 组播屏蔽（mcast_block：按 tag 屏蔽 device 收包）
    void McastSetBlocked(const char *tag, bool blocked);
    bool McastIsBlocked(const char *tag);
    // 虚拟 IP 分配
    uint32_t HostVirtualIp();                                  // 192.168.1.50
    uint32_t DeviceApVirtualIp();                              // 192.168.1.1
    uint32_t DeviceStaVirtualIp(int dev_index);                // 192.168.1.100 + index
    uint16_t DeviceApRealPort(int dev_index);                  // params->device_ap_port_base + index
};
```

### 4.3 虚拟 WiFi 介质语义（sim_wifi.cpp，严格按此实现）

- `wifi_scan`（任意实例调用）：返回全部 active 虚拟 AP（`Modu_XXXX`，band_2g=1，rssi=-50）+ 目标网络（若 `up`）；`count` 超容截断。
- `wifi_ap_start`（device 调用）：生成/刷新 PIN（4 位随机，`pin_consumed=false`）；注册 SimAp（real_port = DeviceApRealPort(索引)，索引由 tag `dev<n>` 解析）；返回 DEMO_OK。
- `wifi_ap_stop`：注销 SimAp；关闭该实例 AP 监听 socket（见 4.4）。
- `wifi_sta_connect`（device 调用，ssid=目标网络或凭据）：查 SimWorld：SSID 不存在或 `!up` → `WIFI_REASON_NO_AP_FOUND`；`auth_fail_injected` 或密码不匹配 → `WIFI_REASON_AUTH_FAIL`；`!band_2g` → `WIFI_REASON_5G_BAND`；成功 → 记录实例虚拟 IP（DeviceStaVirtualIp）+ 返回 OK。
- `wifi_sta_disconnect`：清除 STA 状态（后续 `wifi_get_ip` 返回 0）。
- `wifi_get_rssi`：SimWorld.RssiGet(tag)。
- `wifi_get_ip`：STA 已连 → 设备 STA 虚拟 IP；AP 模式 → DeviceApVirtualIp；host → HostVirtualIp。

### 4.4 真实 socket 语义（sim_socket.cpp）

- 全部 Winsock2；`tcp_listen(port)`：127.0.0.1 监听，非阻塞（WSAEventSelect 或 select 就绪轮询——用**非阻塞 socket + select** 模式，统一事件循环友好），返回句柄。
- `tcp_connect(addr, timeout)`：**地址翻译**：`addr.ip` 为虚拟 IP 时——若 == DeviceApVirtualIp → 目标真实 `127.0.0.1:DeviceApRealPort(匹配的 AP owner_tag 对应索引)`（通过 SimWorld.ApFind(ssid) 反查？——翻译规则：连接 192.168.1.1:5935 时在 SimWorld 中查找 `active` 的 AP（仅一台在配网，host 每次只配一台）取 real_port；若 == HostVirtualIp → `127.0.0.1:params->host_tcp_port`；若 == DeviceStaVirtualIp(i) → 无需翻译（业务中 device 是 client 不连接自己）。真实 IP（127.0.0.1 等）直连。非阻塞 connect + select 等待 timeout；超时 DEMO_ERR_TIMEOUT。
- `sock_recv`：非阻塞 recv；WSAEWOULDBLOCK → DEMO_ERR_AGAIN；对端关闭（recv 0）→ DEMO_ERR（调用方按"连接关闭"处理）。
- `sock_send`：非阻塞 send，WSAEWOULDBLOCK → DEMO_ERR_AGAIN（调用方重试）；0 字节 → DEMO_ERR。
- `udp_mcast_join(group, port)`：socket(AF_INET, SOCK_DGRAM)；`SO_REUSEADDR` 后 bind `0.0.0.0:port`；`IP_ADD_MEMBERSHIP` 加入 group；非阻塞。
- `udp_send`：sendto 组播地址（TTL 默认 1，setsockopt IP_MULTICAST_TTL=1）。
- `udp_recv`：recvfrom；WSAEWOULDBLOCK → DEMO_ERR_AGAIN；**若该实例 McastIsBlocked(tag) 且目标为组播端口 → 丢弃数据继续收（循环到空）**，实现 mcast_block。
- `sock_close`：closesocket；句柄为 `void*`（内部 SOCKET 包装）。
- Winsock 初始化：首次实例创建时 WSAStartup（引用计数），末个销毁时 WSACleanup。

### 4.5 mDNS（sim_mdns.cpp）

- `mdns_register(svc)`：SimWorld.MdnsRegister（type 为 key，重复注册覆盖）。
- `mdns_unregister(type)`、`mdns_resolve(type, out, timeout_ms)`：循环查 SimWorld.MdnsResolve（timeout 内 10ms 间隔轮询，非阻塞友好；找不到返回 DEMO_ERR_TIMEOUT）。

### 4.6 NVS（sim_nvs.cpp）

- 文件：`<tag>.nvs.json`（如 `dev0.nvs.json`）存于 `demo/run/` 目录（目录不存在自动创建；路径由 params->nvs_dir 指定，默认 `run/`，相对可执行文件工作目录）。
- 存储格式：`{"key": "<base64 blob>", ...}`；base64 编解码手写（约 60 行，无第三方依赖）。
- `nvs_get`：键不存在 → DEMO_ERR 且 *len=0；文件损坏 → 备份改名 + 返回 DEMO_ERR（不崩溃）。
- `nvs_set`：覆写文件（写临时文件 + 改名原子替换）。
- 每实例独立文件（多 device 隔离）。

### 4.7 时间/随机/注入

- `time_ms`：`std::chrono::steady_clock` 单调时钟（毫秒，进程内基准）。
- `random`：`std::mt19937`（实例种子 = 设备索引固定 + 时间混合；host 用随机种子）。
- `inject(action, arg_json)`：解析动作与参数，映射到 SimWorld 注入（见设计文档 4.16 表）；`burst_send` 由 device 线程经 `net_inject` 调用自身实例（标记限速触发），sim 后端仅记录日志。

### 4.8 线程模型约束

- SimWorld 方法全部互斥锁（`std::mutex`）；socket 对象仅由所属实例线程使用（device 的 socket 只在其线程内访问；host 的只在其线程内）——**除 SimWorld 外无跨线程共享**。
- `time_ms`/`random` 线程本地安全（实例隔离）。

## 5. 验收标准

1. 构建通过（/W4 /WX），零警告。
2. tests/ 增加 `test_sim_world.cpp`：AP 注册/查 PIN/注销；STA 连接四种 reason（ok/201/202/500）；注入 rssi_set/wifi_disconnect 生效；mDNS 注册解析。
3. tests/ 增加 `test_sim_socket.cpp`：loopback TCP 收发（真实 socket）；组播 join 后自收自发（同一进程两个实例组播互收）；地址翻译（连接 192.168.1.1:5935 实际连通 device AP 监听端口）。
4. tests/ 增加 `test_sim_nvs.cpp`：set/get 往返、键不存在、覆盖、损坏文件恢复（写垃圾字节后 get 不崩溃且返回 DEMO_ERR）。
5. 非阻塞语义验证：对未 listen 端口 connect 返回 DEMO_ERR_TIMEOUT；recv 无数据 DEMO_ERR_AGAIN。
6. 本任务书测试通过后，SubStage_03 视为完成。

## 6. 禁止事项

- 不实现任何 device/host 业务逻辑（仅抽象层后端）。
- 不使用 std::thread 创建线程（线程编排由 Stage 5 main 负责；后端全部函数为同步调用）。
- 不得在头文件泄露 Winsock 类型（对外仅 `void*` 句柄）。
- 不修改 net_abstraction.h 契约。
- 组播绑定时必须 SO_REUSEADDR（同机多实例），否则测试失败。

## 7. 依赖前置

- 等待 SubStage_02 定稿；完成后与 SubStage_04 并行。
- 本任务书产出物供 SubStage_05~11 使用（各任务书按需链接 net_sim 源文件，注意在 CMakeLists 的 provision_demo_core 中加入 net_sim/*.cpp）。
