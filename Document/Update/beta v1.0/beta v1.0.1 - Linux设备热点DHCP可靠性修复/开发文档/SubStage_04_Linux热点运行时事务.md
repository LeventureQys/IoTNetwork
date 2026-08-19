# SubStage 04：Linux 热点运行时事务

- 所属 Stage：Stage 2 - Linux 运行时事务
- 依赖前置：SubStage 01 完成，取得配置与计划接口
- 并行状态：需等待；本任务集中修改 Linux 热点文件，单 Agent 完成
- 所属阶段：阶段三 - 开发

## 1. 目标

实现真实 Linux 单实例、结构化命令执行、rtnetlink 路由采集、动态选段、AP IPv4 回读、隔离 dnsmasq、UDP 67 readiness、可选 NAT和统一事务回滚。

## 2. 当前状态

- `demo/net_linux/linux_hotspot.c:53` 使用 shell 拼接命令。
- `demo/net_linux/linux_hotspot.c:255` 未检查本机路由冲突。
- `demo/net_linux/linux_hotspot.c:321` 只检查配置命令退出码。
- `demo/net_linux/linux_hotspot.c:331` 未使用 `--conf-file=/dev/null`。
- `demo/net_linux/linux_hotspot.c:338` 只检查 PID与 kill -0。
- `demo/net_linux/linux_hotspot.c:347` NAT 不限制源且 stop 不清理。

## 3. 文件范围

允许：

- 新增 `demo/net_linux/linux_hotspot_linux_ops.h`
- 新增 `demo/net_linux/linux_hotspot_linux_ops.c`
- 修改 `demo/net_linux/linux_hotspot.c`
- 修改 `demo/net_linux/linux_hotspot.h`
- 修改 `demo/net_linux/linux_wifi.c/.h`
- 修改 `demo/net_sim/sim_backend.cpp` 的真实 Linux初始化/销毁路径
- 修改 `demo/device/app/main.cpp` 的 backend 创建失败处理
- 修改 `demo/cmake/DemoShared.cmake`
- 新增 `demo/tests/test_linux_hotspot_runtime.cpp`
- 修改 `demo/tests/CMakeLists.txt`

禁止：

- `demo/pc/**`
- `demo/net_win/**`
- `demo/net_esp32c2/**`
- 添加 INPUT/OUTPUT/FORWARD ACCEPT 规则
- 修改 ip_forward
- killall/pkill 系统服务

## 4. Linux ops 契约

至少提供可注入 ops，用于生产实现和非 root 单测：

```c
typedef struct linux_hotspot_ops {
    int (*run_argv)(const char *const argv[]);
    int (*collect_routes)(linux_ipv4_route_t *routes, size_t capacity,
                          size_t *count);
    int (*interface_has_ipv4)(const char *interface_name,
                              const linux_ipv4_cidr_t *expected);
    int (*pidfile_read)(const char *path, pid_t *pid);
    int (*pid_is_expected)(pid_t pid, const char *expected_executable);
    int (*pid_has_udp_listener)(pid_t pid, uint16_t port,
                                uint32_t expected_address);
    uint64_t (*monotonic_ms)(void);
    void (*sleep_ms)(unsigned int milliseconds);
} linux_hotspot_ops_t;
```

生产默认 ops 不暴露到网络抽象 ABI。测试可设置临时 ops；生产启动前必须恢复默认 ops。

## 5. 路由采集

- rtnetlink `RTM_GETROUTE`。
- 只收集 AF_INET、UNICAST、有 OIF、无 gateway、接口 UP 的直连路由。
- 检查所有表；容量不足或解析失败返回错误。
- route 查询失败 fail-closed。

## 6. 单实例

- `<work_dir>/modu_linux_hotspot.lock`，0600，非阻塞 flock。
- init 成功后持有至 deinit。
- lock 文件写 PID且不删除。
- 锁冲突使 backend create 返回 NULL，main 明确退出。
- 模拟模式不获取锁。

## 7. 启动事务

严格按设计文档第 9 节执行。AP 已存在但所有权未知时失败且不删除。hostapd/dnsmasq pidfile 操作前核验 PID进程身份。

所有配置值通过 argv 传入 execvp，不拼入 shell。SSID和密码只写入权限受控的临时 hostapd 配置，不进入额外诊断日志。

## 8. AP 与 DHCP readiness

- hostapd 模式检查保留。
- AP 地址后用 getifaddrs 回读 IPv4和 netmask。
- dnsmasq 使用 `/dev/null` 配置、最终地址/池/掩码、具名 router 和数字 option 54 server-id；数字 54 兼容当前设备 dnsmasq。
- 最多等待 20 次，每次 100 ms。
- PID有效且为当前 dnsmasq；其 `/proc/<pid>/fd` socket inode 必须在 `/proc/net/udp` 对应端口 67，地址为最终 AP 或 0。
- readiness 失败统一回滚。

## 9. NAT

- 默认关闭。
- 开启时要求 ip_forward=1。
- 精确规则包含最终 AP CIDR、STA出口和 comment `modu-provision-nat`。
- 添加前检查，stop 时循环删除完全匹配规则。
- 不创建 FORWARD放行；部署环境负责。

## 10. 回滚

逆序清理 NAT、dnsmasq、pidfile、AP 地址、hostapd、pidfile/配置、AP接口。保留首个根因，cleanup 失败追加日志。stop 幂等，deinit 额外释放锁。

## 11. 初始化传播

- `linux_wifi_init()` 返回热点 init 错误。
- 真实模式 backend 初始化失败时 `sim_backend_create()` 返回 NULL。
- `main.cpp` 检查 backend/net ctx，失败时不创建状态机线程。
- backend destroy 调用热点 deinit。

## 12. 测试

fake ops 覆盖：route 失败/冲突、hostapd失败、AP模式失败、地址回读失败、dnsmasq退出、错误pidfile、错误进程、无UDP67、超时、NAT关闭、ip_forward失败、规则失败、每一阶段逆序清理、幂等stop、未知资源不删除、单实例锁。

## 13. 验收标准

- 全部单测不要求 root。
- 真实 sudo 启动时日志顺序和 readiness 符合设计。
- 失败不报告热点成功，不遗留 TCP listener 或热点资源。
- 不修改部署防火墙放行规则。
