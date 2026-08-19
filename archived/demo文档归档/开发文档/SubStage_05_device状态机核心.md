# SubStage_05：device 状态机核心（device_app + eventlog + limits）

## SubStage: device 状态机核心
- 所属 Stage：Stage 3
- 依赖前置：SubStage_02、SubStage_03（抽象层 + sim 后端可用）
- 并行状态：需等待 SubStage_02/03；本任务书先完成，SubStage_06/07/08 依赖其交付的 device_priv.h 实例化
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

实现嵌入式设备 C 模块的骨架与核心：`device_priv.h`（跨子模块共享结构）、`device_app`（五阶段状态机 + 单线程事件循环）、`device_eventlog`（NVS 持久化环形事件日志）、`device_limits`（设备侧限速）。全部**纯 C99**，零动态内存，单线程事件循环（与 ESP32-C2 单核模型一致）。本任务书完成后，设备可在模拟环境完成 BOOT → STA_JOIN → AP_PROVISION（开热点等待）的状态推进。

## 2. 当前代码状态

- 已有：SubStage_01~03（CMake、common、frame、params、protocol、net_abstraction、sim 后端）。
- 依据：`设计文档.md` 4.7~4.9（device_priv.h 与状态机契约）、4.14（eventlog/limits）、协议文档 4.3（wifi_config 校验）与 6.4（NVS 键）。

## 3. 交付物（全部新建，纯 C99）

```
demo/device/device_priv.h
demo/device/device_app.h
demo/device/device_app.c
demo/device/device_eventlog.h
demo/device/device_eventlog.c
demo/device/device_limits.h
demo/device/device_limits.c
```

## 4. 接口契约

### 4.1 device_priv.h（与设计文档 4.7 完全一致）

```c
#ifndef DEMO_DEVICE_PRIV_H
#define DEMO_DEVICE_PRIV_H
#include "net_abstraction.h"
#include "params.h"
#include "protocol.h"

typedef struct device_app device_app_t;

struct device_app {
    const demo_params_t *params;
    net_ctx_t *net;
    char device_id[18];
    char ap_ssid[33];
    char ap_password[64];
    char ap_pin[8];
    device_state_t state;
    /* 阶段一：凭据 */
    char creds[PROTO_WIFI_CRED_MAX][33];
    char cred_pass[PROTO_WIFI_CRED_MAX][64];
    int  cred_confirmed[PROTO_WIFI_CRED_MAX];
    int  cred_count;
    int  cred_active;
    int  wifi_retry_count;
    int  wifi_backoff_attempt;
    /* 阶段二：配网会话 */
    void *ap_listen;
    void *ap_conn;
    int  ap_pin_fail_count;
    int  provision_auth_ok;
    /* 阶段三：发现 */
    uint64_t last_announce_seq;
    uint64_t discovery_fast_until_ms;
    int  discovery_round;
    net_addr_t sess_host;          /* 发现结果：host 地址 */
    /* 阶段四：会话 */
    void *sess_sock;
    char  session_id[PROTO_SESSION_ID_LEN+1];
    int   heartbeat_interval_ms;
    int   heartbeat_dead_ms;
    uint32_t ping_seq;
    uint64_t last_rx_ms;
    uint64_t last_ping_ms;
    int   malformed_count;
    uint64_t server_time_sync_ms;
    /* 阶段五：自愈 */
    int   reconnect_attempt;
    int   rssi_bad_samples;
    uint64_t rssi_bad_since_ms;
    int   rate_burst_flag;          /* burst_send 注入标记 */
    /* 通用 */
    int   stop_flag;
    uint64_t state_enter_ms;
    uint32_t boot_uptime_s;
    struct { char text[96]; uint32_t boot_s; } evlog[50];
    int evlog_head, evlog_count;
    /* 帧接收缓冲（配网连接与业务连接共用静态缓冲 + 剩余计数） */
    uint8_t rx_buf[PROTO_MSG_MAX_LEN + PROTO_FRAME_HEAD_LEN + 1];
    int rx_len;
    void *rx_sock;                  /* 当前收包 socket（区分配网/业务） */
};
#endif
```

（注意：与设计文档 4.7 相比增加 `sess_host`/`rx_buf`/`rx_len`/`rx_sock`/`rate_burst_flag` 字段，其余一致——设计文档为契约基线，此处为落实补充。）

### 4.2 device_app.h

```c
device_app_t *device_app_create(const demo_params_t *params, net_ctx_t *net,
                                const char *device_id, uint32_t seed);
void device_app_destroy(device_app_t *app);
void device_app_run(device_app_t *app);
void device_app_request_stop(device_app_t *app);
device_state_t device_app_get_state(const device_app_t *app);
const char *device_app_get_pin(const device_app_t *app);
const char *device_app_get_ap_password(const device_app_t *app);
const char *device_app_get_ap_ssid(const device_app_t *app);
uint32_t device_app_uptime_s(const device_app_t *app);
```

### 4.3 状态机与事件循环（device_app.c）

**事件循环**（每轮）：

```c
while (!app->stop_flag) {
    now = net_time_ms(app->net);
    /* 1. 状态机推进（含定时事件：退避到期、心跳到期、超时判定） */
    if (now >= next_tick) { sm_step(app); next_tick = now + 10; }
    /* 2. 当前状态 I/O 轮询（子模块 *_poll，见 SubStage_06/07/08） */
    /* 3. 收包统一入口 device_app_handle_rx（见 4.5） */
    /* 4. 计算 sleep：下一状态迁移/定时事件最小间隔，下限 10ms */
    /* 5. sleep(poll_timeout) —— 通过后端（sim 为 Sleep，真实环境可 select） */
}
```

`sm_step` 内部状态迁移按设计文档 4.9 表实现，关键点：
- **BOOT**：读 NVS（键 `wifi_creds` / `host_candidates` / `evlog`，格式见协议文档 6.4）；schema 校验（值字段 `schema`==1，否则按迁移规则：可解析即用，解析失败清空）；未确认凭据回滚（`cred_confirmed[i]==0` 且存在已确认凭据 → 丢弃未确认组，事件日志 `credential rolled back`）；无凭据 → AP_PROVISION；有 → STA_JOIN；上电错峰：`sleep(rand(0, power_on_jitter_max_ms))` 在 create 内完成（不阻塞状态机，直接 sleep）。
- **STA_JOIN**：`net_wifi_sta_connect(creds[active], pass)`；OK → 事件日志 + DISCOVERY；`AUTH_FAIL`（不可恢复）→ 事件日志 + 下一凭据（`cred_active++`；超限 → AP_PROVISION）；`NO_AP_FOUND/HANDSHAKE_TIMEOUT`（可恢复）→ `wifi_backoff_attempt++` 退避（`base<<n` cap + jitter），`wifi_retry_count++` 达上限 → 下一凭据；全部凭据耗尽 → AP_PROVISION。
- **AP_PROVISION**：调用 `prov_server_start(app)`（SubStage_06）；`prov_server_poll(app)` 处理配网连接；`close_ap` 收到 → 事件日志 + 写 NVS 确认标记 + DISCOVERY；AP 空闲超时（`provision_ap_idle_timeout_ms` 无任何客户端完成认证）→ 关 AP + 事件日志 + 退避 `provision_ap_backoff_ms`（state 停留计时）→ BOOT。
- 其余状态（DISCOVERY/CONNECT/SESSION/HEAL）由 SubStage_07/08 的 poll 函数驱动，本任务书先提供**占位调用点**（`extern int discovery_poll(...)` 等声明 + 未链接时返回 0 的空实现宏——不：本任务书直接声明子模块接口，由 SubStage_06/07/08 提供实现；本任务书编译时子模块尚未存在会链接失败——因此本任务书**暂时不链接**子模块：用 `#ifdef DEMO_SM_SUBMODULES` 条件编译调用点，SubStage_06/07/08 完成后再定义该宏加入调用。MainAgent 在集成时打开）。
- **看门狗**：`now - state_enter_ms > watchdog_state_timeout_ms` 且当前状态非"等待外部输入型"（AP_PROVISION 等待认证/配网、SESSION 等待心跳、HEAL 退避中）→ WARN 日志 + 事件日志（不软重启）。
- 每次状态迁移：`state_enter_ms = now` + INFO 日志 `[DEV:n] state -> X`。

**NVS 键**（device_app.c 内实现读写辅助）：
- `wifi_creds`：JSON 数组 `[{"ssid":..,"password":..,"confirmed":0|1,"schema":1}, ...]`（≤2 组）。
- `host_candidates`：`[{"ip":"192.168.1.50","port":5935,"last_seen":123}, ...]`（≤2 条）。
- `evlog`：JSON 数组 `[{"t":<boot_s>,"m":"..."}, ...]`（≤50 条）。

### 4.4 device_eventlog.h/.c

```c
void evlog_record(device_app_t *app, const char *fmt, ...);
int  evlog_fill_report(device_app_t *app, char *out, int cap);
```

- 环形 50 条（覆盖最旧）；每条 `text ≤ 95 字符` + `boot_s` 时间戳（boot 相对秒，`net_time_ms/1000`）。
- `evlog_record` 同时立即写 NVS（每次全量序列化——Demo 规模小，简化实现；记录频率低，可接受）。
- `evlog_fill_report`：输出最近 10 条，格式 `"[t] msg"` 换行分隔，cap 截断；无事件输出空串。
- 关键事件（调用点）：上电、配网成功/失败、WiFi 断开（reason）、TCP 断开、重连、RSSI 告警、看门狗、凭据回滚、限速丢弃。

### 4.5 device_app_handle_rx（device_app.c，子模块复用）

```c
/* 从 sock 收一帧并分发；is_business=1 业务连接（SESSION），0 配网连接（AP 会话）。
 * 内部：frame_parse 累积 → 畸形计数（>= malformed_max_per_conn 断开）→ cJSON 解析 → 按 cmd 分发。 */
void device_app_handle_rx(device_app_t *app, void *sock, int is_business);
```

- 分发规则（协议文档 2.6 前向兼容）：`auth`/`wifi_config`/`close_ap` → provision 模块（`prov_server_on_msg`，SubStage_06）；`pong`/`diag_query`/`host_ack` → session 模块（SubStage_08）；`app_data` → INFO 日志后丢弃（通用占位，不崩溃）；未知 `vendor_*`/未知命令 → request 型回复通用占位应答 `{"cmd":"<name>","data":{"status":"unsupported"}}`、notification 型仅事件日志——**未知命令不计入畸形计数**；`device_hello`/`ping`/`diag_report` 为对端发起方向，本机收到属协议错误 → 畸形计数。

### 4.6 device_limits.h/.c

```c
int limits_allow_send(device_app_t *app, int is_heartbeat);
```

- 滑动窗口：1 秒窗口内已发送计数（窗口起点 `last_win_ms` + 计数），速率 `params->device_rate_limit_per_sec`；超限且非心跳 → 返回 0（调用方丢弃 + 事件日志，限速事件计数防刷屏：每秒最多 1 条）。
- `rate_burst_flag` 置位时（burst_send 注入）：下一轮窗口内强制触发超限（用于演示，实现为：置位后下一次 allow_send 直接返回 0 并清标记 + 事件日志）。

## 5. 验收标准

1. 纯 C99 编译通过，零警告；加入 provision_demo_core（C 语言编译）。
2. tests/ 增加 `test_eventlog.cpp`：50 条环绕、NVS 持久化往返（create→record→destroy→create→读取一致）、fill_report 最近 10 条。
3. tests/ 增加 `test_limits.cpp`：50 msg/s 窗口、心跳豁免、burst 触发。
4. tests/ 增加 `test_device_sm.cpp`（表驱动，模拟后端注入）：
   - 无凭据 → BOOT → AP_PROVISION（AP 已注册 SimWorld，PIN 4 位，热点名 `Modu_XXXX` = device_id 后 4 位大写）。
   - 有凭据 + 目标网络 up → BOOT → STA_JOIN → DISCOVERY。
   - 有凭据 + 目标网络 auth_fail → STA_JOIN →（不可恢复）→ AP_PROVISION。
   - 凭据回滚：写入未确认凭据 → 重建实例 → BOOT 检测回滚（事件日志含 "rolled back"）。
5. `device_app_run` 在独立线程运行无崩溃（测试用 std::thread 创建/销毁）。
6. 本任务书完成后，设备可独立运行到 AP_PROVISION（模拟环境），日志输出完整。

## 6. 禁止事项

- 不实现配网协议细节（auth/wifi_config 消息处理由 SubStage_06 负责，本任务书只留分发点）。
- 不实现发现/心跳/自愈逻辑（SubStage_07/08）。
- 不使用 malloc/动态内存（产品代码）；不使用 std::thread（测试代码除外）。
- 不使用任何平台 API（Windows/Linux/ESP-IDF 头）——全部经 net_abstraction。
- 不修改 net_abstraction.h / params.h 契约；device_priv.h 为本次新增基准，SubStage_06/07/08 只能读不能改（需改 → 报告 MainAgent）。

## 7. 依赖前置

- 等待 SubStage_02、03 完成后开工。
- 完成后通知 MainAgent；SubStage_06/07/08 基于本任务书交付的 device_priv.h 与 device_app.h 开工。
