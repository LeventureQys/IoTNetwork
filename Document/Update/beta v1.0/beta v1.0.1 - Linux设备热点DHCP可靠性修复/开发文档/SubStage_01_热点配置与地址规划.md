# SubStage 01：热点配置与地址规划

- 所属 Stage：Stage 1 - 纯逻辑与共用契约
- 依赖前置：无
- 并行状态：可与 SubStage 02、03 并行
- 所属阶段：阶段三 - 开发

## 1. 目标

把 Linux 热点配置解析、IPv4/CIDR 校验、DHCP 池校验和候选网段选择拆为不依赖 root 或外部进程的纯逻辑模块，为运行时事务提供已验证的 `linux_hotspot_plan_t`。

## 2. 当前状态

- `demo/net_linux/linux_hotspot.h:23` 无 `prefix_length`。
- `demo/net_linux/linux_hotspot.c:27` 同时承担默认值、JSON 解析和系统启动。
- `demo/net_linux/linux_hotspot.c:33` 默认真实 AP 为 `192.168.1.1`。
- `demo/net_linux/linux_hotspot.c:322` 写死 `/24`。
- 配置缺失或非法 JSON 会静默返回成功。

## 3. 文件范围

允许：

- 新增 `demo/net_linux/linux_hotspot_config.h`
- 新增 `demo/net_linux/linux_hotspot_config.c`
- 修改 `demo/net_linux/linux_hotspot.h`
- 修改 `demo/config/linux_hotspot.json`
- 修改 `demo/cmake/DemoShared.cmake`
- 新增 `demo/tests/test_linux_hotspot_config.cpp`
- 修改 `demo/tests/CMakeLists.txt`

禁止：

- `demo/net_linux/linux_hotspot.c`
- `demo/device/**`
- `demo/pc/**`
- `demo/net_win/**`

## 4. 接口契约

`linux_hotspot_cfg_t` 增加 `int prefix_length`。新增纯逻辑结构：

```c
typedef struct linux_ipv4_cidr {
    uint32_t address;
    uint32_t network;
    uint32_t netmask;
    uint32_t broadcast;
    uint8_t prefix_length;
} linux_ipv4_cidr_t;

typedef struct linux_ipv4_route {
    linux_ipv4_cidr_t destination;
    unsigned int ifindex;
    char interface_name[32];
    unsigned int table;
} linux_ipv4_route_t;

typedef struct linux_hotspot_plan {
    linux_ipv4_cidr_t ap;
    uint32_t dhcp_start;
    uint32_t dhcp_end;
    int candidate_index;
    int used_fallback;
} linux_hotspot_plan_t;
```

所有 `uint32_t` 地址统一使用主机字节序。

至少提供：

```c
void linux_hotspot_cfg_defaults(linux_hotspot_cfg_t *cfg);
int linux_hotspot_cfg_load(linux_hotspot_cfg_t *cfg, const char *path,
                           char *error, size_t error_capacity);
int linux_ipv4_cidr_parse(const char *address, int prefix_length,
                          linux_ipv4_cidr_t *out);
int linux_hotspot_cfg_validate(const linux_hotspot_cfg_t *cfg,
                               char *error, size_t error_capacity);
int linux_hotspot_select_plan(const linux_hotspot_cfg_t *cfg,
                              const linux_ipv4_route_t *routes,
                              size_t route_count,
                              linux_hotspot_plan_t *out,
                              char *error, size_t error_capacity);
void linux_ipv4_format(uint32_t host_order_ip, char *out, size_t capacity);
void linux_netmask_format(uint32_t host_order_mask, char *out, size_t capacity);
```

## 5. 配置规则

- 默认：`10.42.0.1/24`、池 `.100-.200`、`nat=false`。
- 文件必须存在、可读、非空、合法 JSON object、大小不超过 1 MiB。
- 字段缺省使用默认值；字段类型错误、字符串截断必须失败。
- prefix 16 至 30。
- AP、start、end 均为合法 IPv4。
- AP 不得为网络/广播地址。
- 池与 AP 同子网；start <= end；池不得包含 AP、网络或广播地址。
- 接口名只允许字母、数字、`_`、`-`、`.`。

## 6. 候选规则

顺序固定：

1. 配置首选 CIDR。
2. `10.43.0.1/24`。
3. `172.31.250.1/24`。
4. `192.168.250.1/24`。

使用地址区间判断与全部 routes 是否重叠。DHCP start/end 按首选网络中的 host offset 重定位；备用 `/24` 容纳不下时跳过。全部不可用返回错误，不允许回退重叠地址。

## 7. 错误处理

- 任一输出指针为空返回 `DEMO_ERR_INVAL`。
- 失败时输出结构清零。
- error 始终 NUL 结尾，并指出配置路径、字段或冲突候选。
- 不记录 WiFi 密码。

## 8. 测试

覆盖：默认值、prefix 16/24/30、非法 prefix、缺失/空/非法 JSON、类型错误、非法 IP、网络/广播 AP、池逆序/越界/包含 AP、候选顺序、宽窄路由重叠、四候选冲突、池 offset 重定位和备用段容纳失败。

## 9. 验收标准

- 新测试全部通过，不要求 root。
- 模块不调用 shell、netlink、hostapd、dnsmasq 或 iptables。
- 默认 JSON 与默认结构一致。
- 路由输入相同时结果确定且可重复。
