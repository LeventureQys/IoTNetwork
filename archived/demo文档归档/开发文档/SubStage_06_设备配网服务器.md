# SubStage_06：设备配网服务器（device_provision）

## SubStage: 设备配网服务器
- 所属 Stage：Stage 3
- 依赖前置：SubStage_05（device_priv.h / device_app.h 定稿）
- 并行状态：需等待 SubStage_05；可与 SubStage_07/08 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

实现阶段二（SoftAP 配网）全部设备侧逻辑：AP 开启（WPA2 密码 + 一次性 PIN）、配网 TCP 服务器（auth 会话认证 / wifi_config 输入校验 / wifi_result 回复 / close_ap 收尾）、配网确认（连接目标 WiFi 成功才写 NVS，120s 确认窗口）、热点超时与退避。纯 C99。

## 2. 当前代码状态

- 已有：SubStage_01~05。
- 依据：`设计文档.md` 4.10（契约）、4.9（状态机 AP_PROVISION）；`协议文档.md` 4.1（C1~C5 命令定义）、5.1（配网时序）、6.4（NVS 格式）；`流程说明.md` 3.2。

## 3. 交付物（全部新建，纯 C99）

```
demo/device/device_provision.h
demo/device/device_provision.c
```

## 4. 接口契约

```c
/* 进入 AP_PROVISION 时调用：开 AP + 生成 PIN + TCP 监听；返回 DEMO_OK / DEMO_ERR */
int  prov_server_start(device_app_t *app);
/* 事件循环每轮调用一次；处理 accept / 已连接收包（auth/wifi_config/close_ap）；
 * 返回 1=有活动（用于日志节流），0=无 */
int  prov_server_poll(device_app_t *app);
/* 离开状态时调用：关 AP + 关所有连接 */
void prov_server_stop(device_app_t *app);
/* 由 device_app_handle_rx 分发调用：处理一条配网命令（json 已解析） */
void prov_server_on_msg(device_app_t *app, void *json);
```

## 5. 实现细节（严格按此）

### 5.1 启动（prov_server_start）

1. 生成 PIN：4 位数字（`net_random`，`%10000`，不足 4 位补零）；生成 WPA2 密码：`"Modu-" + device_id 后4位 + "-" + 6位随机`（长度 ≥ 8 满足 WPA2）。
2. `net_wifi_ap_start(app->net, ap_ssid, ap_password, ap_pin)`（sim 后端注册虚拟 AP，real_port = 20000+索引）。
3. `net_tcp_listen(app->net, real_port, &app->ap_listen)`（真实端口；AP 逻辑端口 5935 由 sim 翻译）。**注意**：sim 后端 `wifi_ap_start` 已确定 real_port；`prov_server_start` 需从后端取回（sim 提供 `wifi_get_ap_real_port`？——不修改契约：约定 sim 后端在 `wifi_ap_start` 中把监听职责合并——改为：本函数先 `net_tcp_listen(real_port)` 再 `net_wifi_ap_start`，real_port = `params->device_ap_port_base + 索引`（索引从 device_id 尾数字解析）。sim 后端 `ApRegister` 使用同一 real_port 公式，两者一致）。
4. 状态重置：`ap_pin_fail_count=0`、`provision_auth_ok=0`、`ap_conn=NULL`。
5. 事件日志 `ap started, ssid=..., pin=...`（PIN 打印日志 = 模拟"设备标签"）。

### 5.2 轮询（prov_server_poll）

- `ap_listen` 上 `net_tcp_accept`：新连接 → 关闭旧 `ap_conn`（一次仅一个配网客户端）→ 保存 `app->ap_conn` → 记录 `conn_start_ms`（用于 10s 认证超时）。
- `ap_conn` 上有数据：`device_app_handle_rx(app, ap_conn, 0)`（复用收包入口，分发到 prov_server_on_msg）。
- 超时判定（用 `net_time_ms`）：
  - 连接建立后 `provision_auth_timeout_ms` 未完成认证 → 断开该连接（`net_sock_close`，置空 ap_conn）+ 事件日志。
  - 认证完成后 `provision_wifi_cfg_timeout_ms` 未收到 wifi_config → 断开连接（保持热点）+ 事件日志。
- AP 空闲超时（`provision_ap_idle_timeout_ms` 内无任何客户端完成认证）→ 由 device_app 状态机检测（app->state_enter_ms），本模块提供 `prov_server_ap_idle_ms(app)` 辅助查询。

### 5.3 消息处理（prov_server_on_msg）

按 `cmd` 分发：

1. **auth**（未认证时）：
   - `pin` 字段必须为字符串且为 4 位数字；等于 `app->ap_pin` → `provision_auth_ok=1`，回复 `auth_result{ok}`；**PIN 作废**（置 `ap_pin[0]=0`，防重放——同会话重复 auth 直接拒绝）。
   - 错误 → `ap_pin_fail_count++`，回复 `auth_result{fail}` + 断开连接；`ap_pin_fail_count >= provision_pin_fail_max` → 关 AP（`prov_server_stop`）+ 事件日志 `pin locked` + 置标记让状态机进入退避（`app->state` 由 device_app 处理：通过返回值/日志——实现为设置 `app->pin_locked_flag`？——不扩展结构：让 `prov_server_stop` 后 device_app 状态机检查 `ap_pin_fail_count` 超限则进入 BOOT 前退避。简化：**连续 5 次失败直接关 AP 并返回**，状态机下一轮因 `ap_listen==NULL` 且未 close_ap 而按"配网失败"处理进入退避——此路径在 SubStage_05 的任务书中已约定：AP_PROVISION 中 `prov_server_poll` 返回特殊码 `PROV_RET_AP_LOCKED`（=2）时状态机转退避 → BOOT。**契约修正：`prov_server_poll` 返回值：0 无活动 / 1 有活动 / 2 AP 已锁定需退避。**
   - 已认证再收 auth → 畸形计数（未知状态消息）。
2. **wifi_config**（已认证）：
   - 校验：`ssid` 字符串非空 ≤32；`password` 字符串 8~63。失败 → 回复 `wifi_result{fail,"reason":"输入无效"}`（保持连接，可重试），事件日志。
   - 通过 → 尝试连接目标 WiFi：`net_wifi_sta_connect(ssid, password, &reason)`；`AUTH_FAIL` → 回复 `wifi_result{fail,"reason":"密码错误"}`；`NO_AP_FOUND`/`HANDSHAKE_TIMEOUT` → 重试（`provision_sta_try_max` 次，间隔 1s）；全部失败 → `wifi_result{fail,"reason":"信号太弱"}`；`5G_BAND` → `wifi_result{fail,"reason":"目标为5G网络"}`。
   - 成功 → **先写 NVS 后回复**：`wifi_creds` 写为 `[{ssid,password,confirmed:0,schema:1}]`（替换旧值前保留旧组为备选：新组在前 `cred_confirmed=0`，旧组（若存在）在后保持原确认状态）→ 回复 `wifi_result{ok, device_ip}`（IP 由 `net_wifi_get_ip` 格式化）→ 事件日志 `provision ok`。
3. **close_ap**（任意已连接状态）：回复无（协议定义无应答），`prov_server_stop(app)` + 返回标记让状态机迁移 DISCOVERY（通过设置 `app->state` 由 device_app 检测 `ap_listen==NULL && provision_auth_ok` → 迁移；更明确：**本模块设置 `app->provision_done=1`？——不扩展结构：device_app 状态机在调用 poll 后检查 `app->ap_listen==NULL && app->provision_auth_ok` 即迁移 DISCOVERY**（已在 SubStage_05 契约中注明此判定）。
4. 其他 cmd / 未知命令 → 按协议文档 2.6 前向兼容规则（notification 忽略、request 回 unsupported 占位应答，不计畸形；字段/帧错误才计畸形），由 device_app_handle_rx 统一处理。

### 5.4 收尾（prov_server_stop）

- `net_sock_close(ap_conn)`（非空时）；`net_sock_close(ap_listen)`；`net_wifi_ap_stop`；置空句柄；事件日志。

## 6. 验收标准（tests/ 增加 test_provision.cpp）

1. PIN 会话：正确 PIN → auth_result ok 且 PIN 作废（同连接二次 auth 被拒）；错误 PIN → fail；**连续 5 次错误 → AP 锁定（prov_server_poll 返回 2，SimWorld AP 注销）**。
2. 认证超时：连接后超过 provision_auth_timeout_ms（测试用压缩参数 500ms）无 auth → 断开。
3. wifi_config 校验：空 SSID / 33B SSID / 7B 密码 / 64B 密码 → fail（不写 NVS）。
4. 连接成功 → wifi_result ok + device_ip + **NVS 已写且 confirmed=0**。
5. AUTH_FAIL 注入（SimWorld TargetSetAuthFail）→ wifi_result fail 密码错误，NVS 未写。
6. close_ap → 状态机迁移 DISCOVERY（device_app 集成测试，与 test_device_sm 联动）。
7. 全链路：device_app_create →（无凭据）→ AP_PROVISION → host 侧模拟客户端完整配网消息流 → close_ap → DISCOVERY（日志断言）。

## 7. 禁止事项

- 不实现发现/心跳/自愈（SubStage_07/08）。
- 不使用 malloc；不使用平台 API；纯 C99。
- 不修改 device_priv.h（5.1 的契约修正只涉及 prov_server_poll 返回值语义，已在任务书内定死）。
- 不自行解析 JSON 以外的消息格式（帧由 device_app_handle_rx 处理）。

## 8. 依赖前置

- 等待 SubStage_05 交付 device_priv.h / device_app.h / device_app_handle_rx。
- 与 SubStage_07/08 并行；接口契约见第 4 节，互不依赖。
