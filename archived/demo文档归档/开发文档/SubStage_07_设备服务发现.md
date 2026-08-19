# SubStage_07：设备服务发现（device_discovery）

## SubStage: 设备服务发现
- 所属 Stage：Stage 3
- 依赖前置：SubStage_05（device_priv.h / device_app.h 定稿）
- 并行状态：需等待 SubStage_05；可与 SubStage_06/08 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

实现阶段三（三级服务发现）：一级 mDNS 解析 → 二级 UDP 组播监听（host_announce 校验 proto/seq）→ 三级 NVS 候选列表直连；host_bye 即时响应（转候选直连 + 30s 快速窗口）；配网完成后 30s 快速轮询（500ms）。纯 C99。

## 2. 当前代码状态

- 已有：SubStage_01~05。
- 依据：`设计文档.md` 4.11（契约）、4.9（状态机 DISCOVERY）；`协议文档.md` 4.2（C6/C7）、5.2（发现时序）；`流程说明.md` 3.3。

## 3. 交付物（全部新建，纯 C99）

```
demo/device/device_discovery.h
demo/device/device_discovery.c
```

## 4. 接口契约

```c
/* 进入 DISCOVERY 时调用：重置轮次/快速窗口；返回 DEMO_OK */
int  discovery_start(device_app_t *app);
/* 每轮调用；返回 1=已获得 host 地址（写入 app->sess_host），0=继续等待 */
int  discovery_poll(device_app_t *app);
/* 离开状态时调用：关闭组播 socket */
void discovery_stop(device_app_t *app);
/* 由 device_app_handle_rx 的分发调用：处理 host_announce / host_bye（UDP 报文，无帧前缀） */
void discovery_on_udp(device_app_t *app, void *json);
```

## 5. 实现细节

### 5.1 启动（discovery_start）

- `discovery_round = 0`；`discovery_fast_until_ms = now + discovery_fast_window_ms`；`last_announce_seq = 0`。
- 组播 socket：`net_udp_mcast_join(mcast_group, mcast_port, &app->mcast_sock)`。
- 读取 NVS 候选列表（键 `host_candidates`，JSON 数组 `[{"ip","port","last_seen"}]`，≤2 条）到 `app->candidate_list[0..1]`。
- **字段依赖声明**：本模块使用 `device_priv.h` 中的以下字段——`void *mcast_sock`、`uint64_t last_mcast_poll_ms`、`net_addr_t candidate_list[2]`（均由 MainAgent 在集成 SubStage_05 时合并入 `device_priv.h`，SubAgent 不得自行修改头文件；实现时若字段缺失，报告 MainAgent 补加）。

### 5.2 轮询（discovery_poll）

按 `discovery_round` 依次执行，**任一级成功返回 1**：

- **round 0 = mDNS**：`net_mdns_resolve(PROTO_MDNS_TYPE, &svc, 2000)`；成功 → 校验 `svc.addr.port != 0` → `app->sess_host = svc.addr` → 事件日志 `found host via mdns` → 返回 1；失败/超时 → `discovery_round = 1`（下一轮走组播，mDNS 每轮只试一次）。
- **round 1 = 组播**：`net_udp_recv(mcast_sock, buf, cap, &from)`：
  - `DEMO_ERR_AGAIN` → 无数据：检查快速窗口轮询间隔——用时间节流：`discovery_fast_until_ms` 之前每 `discovery_fast_interval_ms` 轮询一次，之后每 `discovery_normal_interval_ms` 轮询一次（记录 `last_mcast_poll_ms`——**补充字段**，同样由 MainAgent 合并，按 `app->last_mcast_poll_ms` 使用）。
  - 收到报文 → `discovery_on_udp` 处理：host_announce 校验通过（`proto == PROTO_VERSION` 且 `seq > last_announce_seq`）→ 解析 `ip`/`tcp_port` → `sess_host`（ip 虚拟地址，sim 翻译层处理）→ 事件日志 → 返回 1。
  - 超时（`discovery_candidate_timeout_ms` 内无结果，用状态驻留计时 `state_enter_ms` 判定）→ `discovery_round = 2`。
- **round 2 = 候选列表**：依次尝试 `candidate_list[0]`、`[1]`（`net_tcp_connect` 快速探测，timeout 500ms，成功即返回 1 并关闭探测连接——真实连接由 CONNECT 状态建立；探测失败换下一候选；全失败 → 回退 `discovery_round = 1` 并退避重试（用 `reconnect_backoff` 语义，上限 30s）。
- 全部轮次失败 → 返回 0（状态机保持 DISCOVERY，事件循环继续）。

### 5.3 host_announce / host_bye（discovery_on_udp）

- `host_bye`：事件日志 `host bye` → 立即尝试候选直连（round=2 优先）+ `discovery_fast_until_ms = now + fast_window_ms`（30s 快速轮询捕捉 host 重启）。
- `host_announce`：`proto` 必须为数字 == PROTO_VERSION（否则忽略，不更新 seq）；`seq` 必须为数字 > `last_announce_seq`（乱序/重复忽略）；`ip`/`tcp_port` 必须存在；通过 → 更新 `last_announce_seq`，返回 host 地址。

### 5.4 收尾（discovery_stop）

- `net_sock_close(mcast_sock)`，置空。

## 6. 验收标准（tests/ 增加 test_discovery.cpp）

1. host 注册 mDNS + 广播 host_announce → device discovery_poll 返回 1 且 sess_host 正确（虚拟 IP 翻译验证：真实连接成功）。
2. announce `proto=2`（不匹配）→ 忽略；`seq` 回退 → 忽略。
3. mDNS 未注册（mdns 失败）→ round 降级到组播成功。
4. 组播屏蔽（McastSetBlocked）→ 候选列表直连成功（预置 NVS 候选）。
5. host_bye → 事件日志 + 候选直连优先。
6. 无任何发现（全部屏蔽 + 无候选）→ 持续返回 0，退避日志正常（上限 30s 压缩后 5s）。

## 7. 禁止事项

- 不实现配网/心跳/自愈（SubStage_06/08）。
- 不使用 malloc；纯 C99；不修改 device_priv.h（所需字段由 MainAgent 在集成时合并，见 5.1 字段依赖声明）。
- 不阻塞调用（mDNS resolve 的 2s 与 TCP 探测 500ms 为允许的短阻塞，其余全部非阻塞轮询）。

## 8. 依赖前置

- 等待 SubStage_05 交付 device_priv.h / device_app.h。
- 与 SubStage_06/08 并行；接口契约见第 4 节，互不依赖；字段合并由 MainAgent 集成时统一处理（SubStage_13 任务书 7 节已列明补充字段清单）。
