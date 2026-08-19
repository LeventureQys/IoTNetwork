/*
 * 硬件抽象层：网络后端接口契约
 *
 * 本文件定义了设备端与硬件网络层之间的抽象边界。
 *
 * 架构设计
 * --------
 * net_backend_t 是一个纯 C 虚表（vtable），包含 29 个函数指针，覆盖 7 个类别。
 * 每种硬件平台（模拟器 / Linux / ESP32-C2）提供各自的 net_backend_t 实例，
 * 上层业务代码（src/core/）通过 net_ctx_t 句柄统一调用，不感知底层平台。
 *
 * 实现新后端只需两步：
 *   1. 实现 net_backend_t 的全部 29 个函数（可参考 backends/sim/sim_backend.c）
 *   2. 调用 net_ctx_create() 将后端注入框架
 * 上层业务代码无需任何修改。
 *
 * 便捷封装（net_xxx 系列）
 * ------------------------
 * net_ctx.h 中定义的 net_wifi_scan(ctx, ...) 等函数是 thin wrapper：
 *   1. 检查 ctx 和对应函数指针是否为 NULL，若是则安全返回 DEMO_ERR
 *   2. 转发调用到 ctx->backend->xxx(ctx->backend_user, ...)
 * 上层代码应统一使用 net_xxx 系列，不要直接调用 backend->xxx。
 */
#ifndef DEMO_NET_ABSTRACTION_H
#define DEMO_NET_ABSTRACTION_H
#include <stdint.h>
#include <stddef.h>
#include "common.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct net_ctx net_ctx_t;
typedef struct net_backend net_backend_t;

/*
 * net_addr_t — IPv4 地址 + 端口
 *   ip   : 网络字节序（大端），调用方负责 htonl/ntohl 转换
 *   port : 网络字节序（大端）
 */
typedef struct net_addr { uint32_t ip; uint16_t port; } net_addr_t;

/*
 * net_ap_info_t — WiFi 热点信息（一次扫描结果中的单条记录）
 *   ssid     : 热点名称，C 字符串，最多 32 字节（含 '\0'）
 *   rssi     : 信号强度，单位 dBm，典型值 -30（强）~ -90（弱）
 *   band_2g  : 1 = 2.4GHz 频段，0 = 5GHz 或其他。ESP32-C2 仅支持 2.4GHz
 *   bssid    : AP MAC 地址，格式 "AA:BB:CC:DD:EE:FF"，17 字节（含 '\0'）
 */
typedef struct net_ap_info { char ssid[33]; int rssi; int band_2g; char bssid[18]; } net_ap_info_t;

/*
 * net_mdns_service_t — mDNS 解析结果
 *   instance : 服务实例名（如 "host"）
 *   type     : 服务类型（如 "_tactile._tcp"）
 *   addr     : 解析得到的 IPv4 地址和端口（网络字节序）
 *   txt      : TXT 记录内容，C 字符串
 */
typedef struct net_mdns_service { char instance[64]; char type[64]; net_addr_t addr; char txt[128]; } net_mdns_service_t;

/*
 * net_backend_t — 后端虚表
 *
 * 定义硬件必须实现的全部网络操作。所有函数第一个参数为 void *user，
 * 即 net_ctx_create() 时传入的 backend_user，供后端携带私有上下文
 * （如 socket 列表、WiFi 状态句柄等）。
 *
 * 返回值约定
 * ----------
 * DEMO_OK        — 成功
 * DEMO_ERR       — 一般失败
 * DEMO_ERR_AGAIN — 非阻塞操作无数据（仅用于 recv/accept）
 * DEMO_ERR_INVAL — 参数无效
 *
 * 各函数指针按类别组织如下。
 */
typedef struct net_backend {

    /* ================================================================
     * 生命周期
     * ================================================================ */

    /*
     * init — 初始化网络栈
     *   config_path : 配置文件路径，可为 NULL 使用默认行为
     *   返回 DEMO_OK 表示初始化成功；失败返回 DEMO_ERR。
     *   init 为空指针（NULL）视为无需初始化，net_ctx_create 仍然成功。
     */
    int  (*init)(void *user, const char *config_path);

    /*
     * deinit — 反初始化，释放网络资源
     *   仅在 init 成功后被 net_ctx_destroy() 调用。
     *   即使 init 为 NULL，deinit 也必须为 NULL（成对）。
     */
    void (*deinit)(void *user);

    /* ================================================================
     * WiFi 介质
     * ================================================================ */

    /*
     * wifi_scan — 扫描周边 WiFi 热点
     *   aps   : 输出数组，调用方预分配
     *   count : [入] aps 数组容量  [出] 实际扫描到的热点数量
     *   返回 DEMO_OK，扫描结果写入 aps[0..count-1]。
     */
    int  (*wifi_scan)(void *user, net_ap_info_t *aps, int *count);

    /*
     * wifi_sta_connect — STA 模式连接指定 WiFi
     *   ssid   : 目标 WiFi 名称，≤32 字节
     *   pass   : 目标 WiFi 密码，8~63 字节（WPA2 约束）
     *   reason : [出] 连接失败时写入 wifi_reason_t
     *            · 0              = 成功
     *            · 201 NO_AP_FOUND = AP 不可见（可恢复，退避重试）
     *            · 202 AUTH_FAIL   = 认证失败（不可恢复，进配网模式）
     *            · 205 HANDSHAKE_TIMEOUT = 握手超时（退避重试）
     *   注意：底层实现可能是异步的，此函数需同步等待结果后返回。
     */
    int  (*wifi_sta_connect)(void *user, const char *ssid, const char *pass, wifi_reason_t *reason);

    /*
     * wifi_sta_disconnect — 断开当前 STA 连接
     */
    int  (*wifi_sta_disconnect)(void *user);

    /*
     * wifi_ap_start — 开启 SoftAP 热点（配网模式）
     *   ssid : 热点名称，约定格式 "Modu_XXXX"（XXXX = MAC 后 4 位大写十六进制）
     *   pass : WPA2-PSK 密码，每设备唯一（PoP 语义）
     *   pin  : 4 位随机数字字符串，一次性配网 PIN，认证成功后立即作废
     *   约定：AP 网关地址 192.168.1.1，最大连接数 1。
     */
    int  (*wifi_ap_start)(void *user, const char *ssid, const char *pass, const char *pin);

    /*
     * wifi_ap_stop — 关闭 SoftAP，切换回 STA 模式
     */
    int  (*wifi_ap_stop)(void *user);

    /*
     * wifi_get_rssi — 获取当前 STA 连接的信号强度
     *   rssi : [出] 信号强度，单位 dBm
     *   -75dBm 为弱信号阈值，设备侧连续 30 秒低于此值触发主动重连。
     */
    int  (*wifi_get_rssi)(void *user, int *rssi);

    /*
     * wifi_get_ip — 获取当前 STA 接口的 IPv4 地址
     *   ip : [出] 网络字节序（大端）的 IPv4 地址
     */
    int  (*wifi_get_ip)(void *user, uint32_t *ip);

    /*
     * wifi_get_current_ssid — 获取当前 STA 实际关联的 WiFi SSID
     *   ssid     : [出] 调用方提供缓冲区
     *   capacity : 缓冲区容量（字节），调用方应提供 ≥33 字节
     */
    int  (*wifi_get_current_ssid)(void *user, char *ssid, int capacity);

    /*
     * wifi_get_gateway — 获取当前 STA 接口的 IPv4 网关地址
     *   ip : [出] 网络字节序（大端）
     */
    int  (*wifi_get_gateway)(void *user, uint32_t *ip);

    /* ================================================================
     * TCP（非阻塞语义）
     *
     * recv/accept 无数据时返回 DEMO_ERR_AGAIN（不是 DEMO_ERR），
     * 调用方应据此区分"暂时无数据"和"真正错误"。
     * connect 阻塞等待，最多 timeout_ms 毫秒。
     *
     * 帧约定：所有 TCP 通道报文使用 2 字节长度前缀（大端序）+ JSON 负载，
     *        单消息负载上限 1024 字节。组帧/解帧由上层 frame 模块处理，
     *        后端只负责原始字节流收发。
     * ================================================================ */

    /*
     * tcp_listen — 在指定端口启动 TCP 服务器
     *   port : 监听端口。配网阶段设备侧端口固定为 PROTO_TCP_PORT (5935)
     *   sock : [出] 监听 socket 句柄
     */
    int  (*tcp_listen)(void *user, uint16_t port, void **sock);

    /*
     * tcp_accept — 接受一个客户端连接
     *   listen : tcp_listen 返回的监听 socket
     *   conn   : [出] 客户端 socket 句柄
     *   peer   : [出] 对端地址（网络字节序），可为 NULL
     *   无客户端连接时返回 DEMO_ERR_AGAIN。
     */
    int  (*tcp_accept)(void *user, void *listen, void **conn, net_addr_t *peer);

    /*
     * tcp_connect — 以 TCP 客户端连接服务器
     *   addr       : 目标地址（网络字节序）
     *   sock       : [出] 连接 socket 句柄
     *   timeout_ms : 连接超时毫秒数，超时返回 DEMO_ERR
     */
    int  (*tcp_connect)(void *user, const net_addr_t *addr, void **sock, int timeout_ms);

    /*
     * sock_send — 发送数据
     *   sock : socket 句柄
     *   buf  : 待发送数据（原始字节）
     *   len  : 数据长度（字节）
     *   返回实际发送字节数，失败返回 DEMO_ERR。
     */
    int  (*sock_send)(void *user, void *sock, const uint8_t *buf, int len);

    /*
     * sock_recv — 接收数据
     *   sock : socket 句柄
     *   buf  : 接收缓冲区
     *   cap  : 缓冲区容量（字节）
     *   返回实际接收字节数；无数据返回 DEMO_ERR_AGAIN；错误返回 DEMO_ERR。
     */
    int  (*sock_recv)(void *user, void *sock, uint8_t *buf, int cap);

    /*
     * sock_close — 关闭 socket
     */
    void (*sock_close)(void *user, void *sock);

    /* ================================================================
     * UDP 组播
     *
     * 组播用于上位机服务发现的兜底通道。
     * 约定地址：224.0.2.1:5936，TTL=1（仅本地网段）。
     * UDP 报文不加长度前缀（天然数据报成帧）。
     * ================================================================ */

    /*
     * udp_mcast_join — 创建 UDP socket 并加入组播组
     *   group : 组播组地址（如 "224.0.2.1"）
     *   port  : 端口（如 5936）
     *   sock  : [出] socket 句柄
     */
    int  (*udp_mcast_join)(void *user, const char *group, uint16_t port, void **sock);

    /*
     * udp_send — 发送 UDP 数据报到组播地址
     *   group : 目标组播地址
     *   port  : 目标端口
     *   buf   : 待发送数据
     *   len   : 数据长度
     */
    int  (*udp_send)(void *user, const char *group, uint16_t port, const uint8_t *buf, int len);

    /*
     * udp_recv — 接收组播数据报
     *   sock : udp_mcast_join 返回的 socket
     *   buf  : 接收缓冲区
     *   cap  : 缓冲区容量
     *   from : [出] 发送方地址（网络字节序），可为 NULL
     *   无数据返回 DEMO_ERR_AGAIN。
     */
    int  (*udp_recv)(void *user, void *sock, uint8_t *buf, int cap, net_addr_t *from);

    /* ================================================================
     * mDNS
     *
     * mDNS 是服务发现的首选通道。
     * 约定服务类型：PROTO_MDNS_TYPE = "_tactile._tcp"
     * 约定实例名：   PROTO_MDNS_INSTANCE = "host"
     * ================================================================ */

    /*
     * mdns_register — 注册 mDNS 服务
     *   svc : 服务信息（类型、实例名、地址、TXT 记录）
     */
    int  (*mdns_register)(void *user, const net_mdns_service_t *svc);

    /*
     * mdns_unregister — 注销 mDNS 服务
     *   type : 服务类型（如 "_tactile._tcp"）
     */
    int  (*mdns_unregister)(void *user, const char *type);

    /*
     * mdns_resolve — 解析 mDNS 服务，获取 IP 和端口
     *   type       : 服务类型（如 "_tactile._tcp"）
     *   out        : [出] 解析结果
     *   timeout_ms : 解析超时毫秒数，超时返回 DEMO_ERR
     */
    int  (*mdns_resolve)(void *user, const char *type, net_mdns_service_t *out, int timeout_ms);

    /* ================================================================
     * NVS（Non-Volatile Storage，持久化键值存储）
     *
     * 用于保存 WiFi 凭据、主机候选列表、事件日志等掉电不丢失的配置。
     *
     * 约定命名空间：  "provision"
     * 约定键名：
     *   "wifi_creds"      — WiFi 凭据集，JSON 字节流，最多 2 组（主/备）
     *   "host_candidates" — 主机候选列表，JSON 字节流，最多 2 条（IP+port+last_seen）
     *   "evlog"           — 事件日志环形缓冲，JSON 字节流，最多 50 条
     *
     * 键不存在时 nvs_get 返回 DEMO_ERR 且 *len=0（不是错误，仅表示"未写入过"）。
     * ================================================================ */

    /*
     * nvs_get — 读取键值
     *   key : 键名（C 字符串）
     *   buf : [出] 值缓冲区
     *   len : [入] 缓冲区容量  [出] 实际值长度（字节）
     *   键不存在时返回 DEMO_ERR，*len=0。
     */
    int  (*nvs_get)(void *user, const char *key, uint8_t *buf, int *len);

    /*
     * nvs_set — 写入键值
     *   key : 键名
     *   buf : 值数据
     *   len : 值长度（字节）
     */
    int  (*nvs_set)(void *user, const char *key, const uint8_t *buf, int len);

    /*
     * nvs_erase — 删除键
     */
    int  (*nvs_erase)(void *user, const char *key);

    /* ================================================================
     * 系统函数
     * ================================================================ */

    /*
     * time_ms — 获取系统毫秒时间戳
     *   返回自系统启动以来的毫秒数（uint64_t），用于超时计算和心跳定时。
     */
    uint64_t (*time_ms)(void *user);

    /*
     * random — 获取随机数
     *   返回 uint32_t 随机值。
     *   用途：生成一次性 PIN 码（4 位数字）、上电错峰延迟（0~10 秒）。
     */
    uint32_t (*random)(void *user);

    /* ================================================================
     * 故障注入（仅开发/测试用）
     *
     * inject 仅 sim 后端实现，用于模拟网络故障。
     * 真实硬件后端（linux / esp32c2）应返回 DEMO_ERR 表示不支持。
     * ================================================================ */

    /*
     * inject — 注入故障事件
     *   action   : 故障类型（如 "disconnect", "timeout", "rssi_low"）
     *   arg_json : 故障参数，JSON 字符串
     *   仅 sim 后端支持。其他后端返回 DEMO_ERR。
     */
    int  (*inject)(void *user, const char *action, const char *arg_json);

} net_backend_t;

/*
 * net_ctx_t — 网络上下文句柄
 *
 * 封装了 net_backend_t 指针 + backend_user 指针，
 * 上层代码通过 net_xxx(ctx, ...) 系列函数间接调用后端，
 * 获得统一的 NULL 安全检查、错误处理和日志。
 *
 * net_ctx_create
 *   backend      : 后端虚表指针（不可为 NULL）
 *   backend_user : 后端私有上下文（如 socket 列表句柄），所有权归调用方
 *   config_path  : 配置文件路径，传给 backend->init()
 *   out_ctx      : [出] 创建的上下文句柄
 *   返回 DEMO_OK 成功；失败时 *out_ctx 为 NULL 且不会调用 deinit。
 *
 * net_ctx_destroy
 *   ctx : 要释放的上下文。如果 backend->init 曾成功，会先调用 backend->deinit()。
 *         ctx 为 NULL 时无操作。
 */
int net_ctx_create(const net_backend_t *backend,
                   void *backend_user,
                   const char *config_path,
                   net_ctx_t **out_ctx);
void net_ctx_destroy(net_ctx_t *ctx);

/*
 * 以下为便捷封装函数。
 *
 * 每个函数的语义、参数和返回值与 net_backend_t 中对应函数指针相同。
 * 额外行为：
 *   1. ctx 为 NULL → 安全返回 DEMO_ERR
 *   2. 对应函数指针为 NULL → 安全返回 DEMO_ERR
 *   3. 否则转发到 ctx->backend->xxx(ctx->backend_user, ...)
 *
 * 上层代码应统一使用 net_xxx 系列，不要直接调用 backend->xxx。
 */
int  net_wifi_scan(net_ctx_t *c, net_ap_info_t *aps, int *count);
int  net_wifi_sta_connect(net_ctx_t *c, const char *ssid, const char *pass, wifi_reason_t *reason);
int  net_wifi_sta_disconnect(net_ctx_t *c);
int  net_wifi_ap_start(net_ctx_t *c, const char *ssid, const char *pass, const char *pin);
int  net_wifi_ap_stop(net_ctx_t *c);
int  net_wifi_get_rssi(net_ctx_t *c, int *rssi);
int  net_wifi_get_ip(net_ctx_t *c, uint32_t *ip);
int  net_wifi_get_current_ssid(net_ctx_t *c, char *ssid, int capacity);
int  net_wifi_get_gateway(net_ctx_t *c, uint32_t *ip);
int  net_tcp_listen(net_ctx_t *c, uint16_t port, void **sock);
int  net_tcp_accept(net_ctx_t *c, void *listen, void **conn, net_addr_t *peer);
int  net_tcp_connect(net_ctx_t *c, const net_addr_t *addr, void **sock, int timeout_ms);
int  net_sock_send(net_ctx_t *c, void *sock, const uint8_t *buf, int len);
int  net_sock_recv(net_ctx_t *c, void *sock, uint8_t *buf, int cap);
void net_sock_close(net_ctx_t *c, void *sock);
int  net_udp_mcast_join(net_ctx_t *c, const char *group, uint16_t port, void **sock);
int  net_udp_send(net_ctx_t *c, const char *group, uint16_t port, const uint8_t *buf, int len);
int  net_udp_recv(net_ctx_t *c, void *sock, uint8_t *buf, int cap, net_addr_t *from);
int  net_mdns_register(net_ctx_t *c, const net_mdns_service_t *svc);
int  net_mdns_unregister(net_ctx_t *c, const char *type);
int  net_mdns_resolve(net_ctx_t *c, const char *type, net_mdns_service_t *out, int timeout_ms);
int  net_nvs_get(net_ctx_t *c, const char *key, uint8_t *buf, int *len);
int  net_nvs_set(net_ctx_t *c, const char *key, const uint8_t *buf, int len);
int  net_nvs_erase(net_ctx_t *c, const char *key);
uint64_t net_time_ms(net_ctx_t *c);
uint32_t net_random(net_ctx_t *c);
int  net_inject(net_ctx_t *c, const char *action, const char *arg_json);

#ifdef __cplusplus
}
#endif

#endif
