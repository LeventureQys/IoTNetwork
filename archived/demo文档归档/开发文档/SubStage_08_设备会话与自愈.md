# SubStage_08：设备会话/心跳与自愈（device_session + device_heal）

## SubStage: 设备会话与自愈
- 所属 Stage：Stage 3
- 依赖前置：SubStage_05（device_priv.h / device_app.h 定稿）
- 并行状态：需等待 SubStage_05；可与 SubStage_06/07 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

实现阶段四（注册握手 + 心跳保活 + 帧解析）与阶段五（异常自愈：指数退避 + 抖动重连、busy 长退避、RSSI 监测、降级链、候选列表更新）的全部设备侧逻辑。纯 C99。

## 2. 当前代码状态

- 已有：SubStage_01~05。
- 依据：`设计文档.md` 4.12/4.13（契约）、4.9（状态机 CONNECT/SESSION/HEAL）；`协议文档.md` 4.3（C8~C13）、5.3/5.4（时序）、6.3（版本协商）；`流程说明.md` 3.4/3.5。

## 3. 交付物（全部新建，纯 C99）

```
demo/device/device_session.h
demo/device/device_session.c
demo/device/device_heal.h
demo/device/device_heal.c
```

## 4. 接口契约

```c
/* —— device_session —— */
/* 建立业务连接（host 地址来自发现）并发送 device_hello（重连携带 session_id）；0=ok */
int  session_connect(device_app_t *app, const net_addr_t *host);
/* 每轮调用：收包 / 心跳发送 / 判死；返回 1=连接仍存活，0=连接失效（进入 HEAL） */
int  session_poll(device_app_t *app);
/* 关闭业务连接（HEAL 前调用） */
void session_disconnect(device_app_t *app);
/* 由 device_app_handle_rx 分发：pong / diag_query / host_ack 处理 */
void session_on_msg(device_app_t *app, void *json);

/* —— device_heal —— */
/* 断线登记：事件日志 + 候选列表更新（当前 host 置顶）；reason 为日志用文本 */
void heal_on_disconnect(device_app_t *app, const char *reason);
/* 指数退避：base×2^n（cap 截断）+ rand(0,jitter)；n 为 app->reconnect_attempt，调用后递增 */
int  heal_next_backoff_ms(device_app_t *app);
void heal_reset_backoff(device_app_t *app);
/* RSSI 采样：每 rssi_sample_interval_ms 采一次；连续劣化（< 阈值 持续 rssi_bad_duration_ms）→ 事件日志 + 返回 1（需主动重连） */
int  heal_rssi_poll(device_app_t *app);
```

## 5. 实现细节

### 5.1 session_connect（CONNECT 状态调用）

1. `net_tcp_connect(host, &sock, hello_timeout_ms)`；失败 → 事件日志 + 返回 DEMO_ERR（状态机转 HEAL 退避）。
2. 构造 `device_hello`（cJSON）：`id`=device_id、`type`="pressure_sensor"、`fw_version`=params->device_fw_version、`proto_ver`=params->device_proto_ver、`capabilities`=["pressure"]、`uptime`=device_app_uptime_s；`app->session_id[0] != 0`（上次会话）→ 附加 `session_id`。
3. `frame_wrap` + `net_sock_send`；发送失败 → 断开返回 DEMO_ERR。
4. 状态：`last_rx_ms = now`、`last_ping_ms = 0`、`ping_seq = 0`、`malformed_count = 0`、`heartbeat_interval_ms = params->heartbeat_interval_ms`（待 ack 覆盖）、`heartbeat_dead_ms = params->heartbeat_dead_ms ? : interval*3/2`。
5. 返回 DEMO_OK。

### 5.2 session_poll（SESSION 状态每轮调用）

1. `net_sock_recv`：`DEMO_ERR_AGAIN` → 无数据；`DEMO_ERR` → 连接关闭 → 返回 0；有数据 → `device_app_handle_rx(app, sess_sock, 1)`（分发到 session_on_msg）。
2. 心跳：`now - last_ping_ms >= heartbeat_interval_ms` → 构造 `ping{seq:++ping_seq}`（frame_wrap + send）；**限速检查**：`limits_allow_send(app, 1)` 心跳豁免恒放行。
3. 判死：`now - last_rx_ms > heartbeat_dead_ms` → 事件日志 `heartbeat dead` → 返回 0。
4. 收到 `host_ack`（协议上 device 不应收 ack，视为畸形；host_ack 由 host 在 hello 后回复——**修正**：host_ack 确实由 device 接收！分发到 session_on_msg 的 `host_ack` 分支）：
   - `status=ok`：保存 `session_id`（host 下发的）、`heartbeat_interval_ms`（×1000 转 ms）、`server_time` 校时（`server_time_sync_ms = now - server_time*1000`，仅日志换算用）、`proto_ver`（日志记录生效版本）；事件日志 `session established`。
   - `status=busy`：事件日志 `host busy` → `session_disconnect` → 状态机进入 HEAL（busy 长退避：`heal_next_backoff_ms` 用 busy_backoff_ms 固定值——实现：`heal_on_busy(app)` 设置 `app->busy_backoff_pending_ms`？——简化：heal 模块提供 `heal_next_backoff_ms` 在 `app->reconnect_attempt < 0` 时返回 busy_backoff_ms（由 session_on_msg 调 `heal_set_busy(app)` 置 `reconnect_attempt = -1`）——**字段补充：`busy_pending` 由 MainAgent 合并到 device_priv.h**，本模块按 `app->busy_pending` 使用）。
   - `status=fail`：事件日志（含 reason）→ 返回 0（转 HEAL，按普通退避）。
5. 收 `diag_query` → 构造 `diag_report`（uptime/rssi（net_wifi_get_rssi）/state=device_state_str/error_count（wifi_disconnects、tcp_drops 计数——**字段补充：`err_wifi_disconnects`/`err_tcp_drops`/`err_auth_fails` 由 MainAgent 合并**）/events=evlog_fill_report/last_event_time）→ send。
6. 返回 1。

### 5.3 session_disconnect

- `net_sock_close(sess_sock)` 置空；记录 `err_tcp_drops`（若因异常断开）。

### 5.4 heal（HEAL 状态）

- 状态机进入 HEAL 时：`heal_on_disconnect(reason)`：事件日志 + 候选列表更新（把当前 `sess_host` 写入 NVS `host_candidates` 首位，保留旧值次位）。
- 重连循环：`heal_next_backoff_ms()`（base=reconnect_backoff_base_ms，`n = reconnect_attempt++`，cap=reconnect_backoff_cap_ms，+ jitter；`busy_pending` 时返回 busy_backoff_ms 并清标记）→ 等待退避 → `session_connect(sess_host)`：成功 → SESSION；失败 → 继续退避；`now - state_enter_ms > reconnect_to_discovery_ms` → 事件日志 `reconnect timeout, back to discovery` → 状态机迁移 DISCOVERY。
- `heal_rssi_poll`：间隔采样 `net_wifi_get_rssi`；`< rssi_bad_threshold_dbm` → `rssi_bad_since_ms` 首次记录（连续计时）；恢复 → 清零；连续劣化时长 ≥ `rssi_bad_duration_ms` → 事件日志 `rssi degraded` → 清计时 + 返回 1（状态机发起主动重连：断开 → HEAL 重连）。
- WiFi 断开（reason 处理）由 device_app 状态机在 STA_JOIN 语义中处理（session/heal 不涉及）。

## 6. 验收标准（tests/ 增加 test_session.cpp）

1. 完整注册：connect + hello 发出（抓包日志/模拟 host 收帧）→ host_ack ok → 保存 session_id/heartbeat 覆盖（2s）→ 按 2s 发 ping。
2. 判死：host 不回应（收包冻结）→ heartbeat_dead（3s）→ session_poll 返回 0 → HEAL 事件日志。
3. 粘包/畸形：host 一次发送两帧 → 均正确解析；连续 3 次畸形帧 → 连接被断开。
4. busy：host_ack busy → 断开 + 事件日志 + 下次退避 = busy_backoff_ms。
5. fail：host_ack fail → 断开 + 转 HEAL。
6. diag_query → diag_report 字段齐全（uptime/rssi/state/error_count/events/last_event_time）。
7. 退避序列：heal_next_backoff_ms 依次 ≥ base×2^n（1s,2s,4s,8s…cap 5s）+ 抖动范围 [0,jitter]；reset 后从 base 开始。
8. RSSI 注入 -80（rssi_set）→ 连续采样超过 rssi_bad_duration_ms（测试压缩 1s）→ 返回 1 + 事件日志；恢复 -50 → 不再告警。

## 7. 禁止事项

- 不实现配网/发现（SubStage_06/07）。
- 不使用 malloc；纯 C99；不修改 device_priv.h（所需补充字段 `busy_pending`、`err_wifi_disconnects`、`err_tcp_drops`、`err_auth_fails` 以注释标注"MainAgent 合并"）。
- 不阻塞调用（仅 connect/发送允许短阻塞）。
- 不自行实现 JSON 编解码（用 cJSON）。

## 8. 依赖前置

- 等待 SubStage_05 交付 device_priv.h / device_app.h / device_app_handle_rx。
- 与 SubStage_06/07 并行；接口契约见第 4 节。
