# SubStage_02：网络抽象层接口定义

## SubStage: 网络抽象层接口定义
- 所属 Stage：Stage 2
- 依赖前置：SubStage_01（common.h、protocol.h 可用）
- 并行状态：需等待 SubStage_01；完成后 SubStage_03/04 可并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

定义纯 C99 网络抽象层接口 `include/net_abstraction.h`（vtable 后端模式），作为 device（纯 C）与 host（C++）访问一切网络/存储/系统能力的**唯一通道**。接口按 BSD socket 语义设计，使 esp32c2 后端可近乎 1:1 映射 ESP-IDF（lwIP socket + esp_wifi + mdns + NVS）。本任务书只产出头文件，不实现任何后端。

## 2. 当前代码状态

- `demo/` 已有 SubStage_01 交付物（CMake、common.h、protocol.h 等）。
- 依据：`设计文档.md` 4.6（完整契约）、4.4（frame）、2.3（虚拟网络语义）。

## 3. 交付物

```
demo/include/net_abstraction.h
```

## 4. 接口契约（完整复制，不得增删）

```c
#ifndef DEMO_NET_ABSTRACTION_H
#define DEMO_NET_ABSTRACTION_H
#include <stdint.h>
#include <stddef.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct net_ctx net_ctx_t;
typedef struct net_backend net_backend_t;

typedef struct net_addr { uint32_t ip; uint16_t port; } net_addr_t;
typedef struct net_ap_info { char ssid[33]; int rssi; int band_2g; char bssid[18]; } net_ap_info_t;
typedef struct net_mdns_service { char instance[64]; char type[64]; net_addr_t addr; char txt[128]; } net_mdns_service_t;

typedef struct net_backend {
    int  (*init)(void *user, const char *config_path);
    void (*deinit)(void *user);
    int  (*wifi_scan)(void *user, net_ap_info_t *aps, int *count);
    int  (*wifi_sta_connect)(void *user, const char *ssid, const char *pass, wifi_reason_t *reason);
    int  (*wifi_sta_disconnect)(void *user);
    int  (*wifi_ap_start)(void *user, const char *ssid, const char *pass, const char *pin);
    int  (*wifi_ap_stop)(void *user);
    int  (*wifi_get_rssi)(void *user, int *rssi);
    int  (*wifi_get_ip)(void *user, uint32_t *ip);
    int  (*tcp_listen)(void *user, uint16_t port, void **sock);
    int  (*tcp_accept)(void *user, void *listen, void **conn, net_addr_t *peer);
    int  (*tcp_connect)(void *user, const net_addr_t *addr, void **sock, int timeout_ms);
    int  (*sock_send)(void *user, void *sock, const uint8_t *buf, int len);
    int  (*sock_recv)(void *user, void *sock, uint8_t *buf, int cap);
    void (*sock_close)(void *user, void *sock);
    int  (*udp_mcast_join)(void *user, const char *group, uint16_t port, void **sock);
    int  (*udp_send)(void *user, const char *group, uint16_t port, const uint8_t *buf, int len);
    int  (*udp_recv)(void *user, void *sock, uint8_t *buf, int cap, net_addr_t *from);
    int  (*mdns_register)(void *user, const net_mdns_service_t *svc);
    int  (*mdns_unregister)(void *user, const char *type);
    int  (*mdns_resolve)(void *user, const char *type, net_mdns_service_t *out, int timeout_ms);
    int  (*nvs_get)(void *user, const char *key, uint8_t *buf, int *len);
    int  (*nvs_set)(void *user, const char *key, const uint8_t *buf, int len);
    int  (*nvs_erase)(void *user, const char *key);
    uint64_t (*time_ms)(void *user);
    uint32_t (*random)(void *user);
    int  (*inject)(void *user, const char *action, const char *arg_json);
} net_backend_t;

net_ctx_t *net_ctx_create(const net_backend_t *be, void *user);
void net_ctx_destroy(net_ctx_t *ctx);

int  net_wifi_scan(net_ctx_t *c, net_ap_info_t *aps, int *count);
int  net_wifi_sta_connect(net_ctx_t *c, const char *ssid, const char *pass, wifi_reason_t *reason);
int  net_wifi_sta_disconnect(net_ctx_t *c);
int  net_wifi_ap_start(net_ctx_t *c, const char *ssid, const char *pass, const char *pin);
int  net_wifi_ap_stop(net_ctx_t *c);
int  net_wifi_get_rssi(net_ctx_t *c, int *rssi);
int  net_wifi_get_ip(net_ctx_t *c, uint32_t *ip);
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
```

## 5. 语义约束（头文件注释中必须写明）

1. 全部便捷封装函数转发到 `ctx->backend`，NULL 检查（ctx/backend/函数指针为 NULL → 返回 DEMO_ERR 或安全默认值）。
2. `net_addr_t.ip`/`port` 网络字节序；`net_wifi_get_ip` 输出网络字节序。
3. 非阻塞语义：`sock_recv`/`udp_recv` 无数据返回 `DEMO_ERR_AGAIN`；`tcp_accept` 无新连接返回 `DEMO_ERR_AGAIN`；其余错误返回负错误码。
4. `wifi_scan`：`count` 入参为数组容量，出参为实际数量；扫描结果含 SSID 长度 >32 截断。
5. `tcp_connect` 阻塞等待连接结果，`timeout_ms<=0` 表示默认 3000ms；超时返回 `DEMO_ERR_TIMEOUT`。
6. `mdns_resolve` 阻塞最多 `timeout_ms`；未找到返回 `DEMO_ERR_TIMEOUT`。
7. `nvs_get`：`len` 入=缓冲区容量，出=实际长度；键不存在返回 `DEMO_ERR_TIMEOUT` 之外的专用码 `DEMO_ERR` 并在出参置 0——改为：键不存在返回 `DEMO_ERR` 且 `*len=0`（调用方按此判断）。
8. `inject`：仅 sim 后端实现；esp32c2 骨架返回 `DEMO_ERR`。
9. 所有 `char*` 入参允许 NULL 的仅 `pin`（AP 可无密码？本 Demo 必填，NULL → DEMO_ERR_INVAL）与 `arg_json`（inject 可空）。

## 6. 验收标准

1. 头文件以 C99 编译器（MSVC /TC）与 C++17 编译器均编译通过，零警告（/W4 /WX）。
2. 提供最小验证：在 tests/ 增加 `test_net_abstraction.cpp`，用 NULL 后端调用便捷封装（预期 DEMO_ERR 安全返回，不崩溃）；含空指针成员的后端表（函数指针 NULL）调用不崩溃。
3. 头文件注释完整覆盖第 5 节语义约束。
4. 不包含任何平台头（windows.h/winsock2.h 等），仅标准 C 头。

## 7. 禁止事项

- 不实现任何后端（sim/esp32c2 由 SubStage_03/04 负责）。
- 不引入 C++ 语法（接口必须 C/C++ 双可用）。
- 不修改 common.h/protocol.h 已有定义；若确需新增通用类型，报告 MainAgent。
- 不得把 `net_backend_t` 定义为抽象基类（必须保持 C 结构体 vtable，device 是纯 C）。

## 8. 依赖前置

- 等待 SubStage_01 完成后开工；所需头文件：`common.h`（DEMO_ERR 系列）、`protocol.h`（wifi_reason_t）。
- 完成后通知 MainAgent，SubStage_03（sim 后端）与 SubStage_04（esp32c2 骨架）即可并行开工。
