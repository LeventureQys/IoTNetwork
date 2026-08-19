# device/backends/sim - 设备模拟后端（纯 C11）

本目录是设备目录内的纯 C、实例化、可测试模拟后端。所有生产文件以 C11 编译，
无 `.cpp`、Qt、`std::`、class/new/delete、singleton 或进程级可变全局状态。

## 模块清单

| 文件 | 职责 |
|---|---|
| `sim_backend.c` | 上下文 `sim_backend_t`、29 项 `net_backend_t` vtable、工厂 `device_sim_backend_create` 与装配/逆序释放 |
| `sim_world.c` | 目标 WiFi / 虚拟 AP / RSSI / mDNS 注册表 / 组播屏蔽的显式实例状态（内部锁，线程安全） |
| `sim_socket.c` | TCP/UDP/组播非阻塞 socket 与平台错误映射（`WSAGetLastError`/`errno` → `DEMO_*`） |
| `sim_tcp_endpoint.c` | 模拟地址 → loopback 纯函数（真实模式不翻译；仅 PC 热点目标翻译） |
| `sim_nvs.c` | JSON blob 键值存储、严格 base64、同目录 tmp + flush + 原子替换 |
| `sim_ap_catalog.c` | 跨进程 PC 热点 catalog 读取（设计文档 7.3 schema 2，设备侧只读） |
| `sim_mdns.c` | mDNS 注册/注销/超时轮询解析（v1.1 业务已移除，vtable 保留） |
| `sim_fault.c` | 故障注入 action 分派与发送失败计数 |
| `sim_random.c` | 实例化 xorshift32（seed=0 自动播种，非零 seed 可复现） |
| `sim_util.h` | 内部共享平台工具（时钟/休眠/PID/原子写/路径/IPv4），不构成模块 |

## 工厂契约（设计文档 8.2.1）

`device_sim_backend_create(options, &out_instance, &error)`：

- 进入时清零 `out_instance`；成功时 `vtable/user/destroy_user` 均非空。
- 失败返回明确 `device_result_t`（INVALID_ARGUMENT / NO_MEMORY / BACKEND_INIT），
  `out_instance` 保持全零，无部分实例。
- options 字符串仅在调用期借用，内部复制。
- `sim_catalog_dir` 必须为显式绝对路径，否则创建失败。
- `device_index` 范围 0～15（用于 tag / NVS 默认路径 / STA 虚拟 IP 推导）。
- `random_seed==0` 自动播种，非零 seed 确定性复现。
- `nvs_file` 为空时回退默认 `run/dev<index>.nvs.json`。
- 实例销毁走 `out_instance->destroy_user`（幂等，可接收 NULL）。

## PC 热点 catalog 契约（设计文档 7.3，schema 2，冻结）

beta v1.1 角色反转：**PC 是唯一 publisher，设备只读取**。设备不发布、
不删除 PC 记录，也不创建 `device-<index>.json`。

- 文件名 `<sim_catalog_dir>/pc-hotspot.json`（单一文件）。
- 发布端（PC）写入使用同目录 tmp + `fflush` + 原子替换；读取端通过
  `sim_util_read_file`（Windows `FILE_SHARE_DELETE`）并发安全地读取正式文件。
- JSON 字段（类型严格，未知字段忽略，损坏/缺失/越界/非法 IP 记录被忽略）：
  `schema=2`、`ssid`、`password`、`logical_gateway`、`prefix_length`、
  `tcp_port`、`loopback_host`、`loopback_port`、`published_at_ms`（墙钟 ms）、
  `owner_pid`。
- 时效：记录超过 30 秒且 `owner_pid` 已不存在 → 忽略；无法判断 PID 时仅用 30 秒时效。

## v1.1 设备 sim 行为

- `wifi_scan`：catalog 存在有效记录时作为一条 2.4GHz AP 返回（`band_2g=1`）；
  无记录不返回任何 AP。
- `wifi_sta_connect`：精确匹配 `ssid`；密码不符返回 `WIFI_REASON_AUTH_FAIL(202)`；
  无记录/SSID 不符返回 `WIFI_REASON_NO_AP_FOUND(201)`。
- `wifi_get_gateway`：STA 已连接时返回 `192.168.137.1`（`PROTO_PC_AP_IP`）。
- `tcp_connect`：`192.168.137.1:5935` 命中时经 catalog 翻译到
  `127.0.0.1:<loopback_port>`；其余地址不做该翻译；PC 未发布（无有效记录）时
  固定目标连接 fail-closed 返回 `DEMO_ERR`。
- `wifi_ap_start/stop`：返回 `DEMO_ERR`（v1.1 设备不再创建热点）。
- 密码不得写入日志；本后端没有任何日志输出密码的路径。

## 行为保持与决策记录

- vtable 保持 `net_abstraction.h` 29 项布局；init 恒成功（WSA 在 create 完成）。
- inject action 名称保持不变：`wifi_disconnect/wifi_ok/wifi_auth_fail/wifi_auth_ok/
  wifi_ssid_mismatch/rssi_set/mcast_block/mcast_unblock/burst_send/sock_send_fail`；
  未知 action 返回 `DEMO_ERR`。注意：`wifi_disconnect/wifi_ok/wifi_auth_fail/wifi_auth_ok`
  仍被接受（返回 `DEMO_OK`），但 v1.1 连接路径由 catalog 驱动，这四个 action
  不再影响 `wifi_sta_connect` 结果（属于 UI/运行时的语义清理范围）。
- NVS：非法 base64 失败；缓冲不足返回错误并报所需长度，不截断。
- 设备 STA 虚拟 IP：`192.168.137.(100+index)`；PC 网关虚拟 IP：`192.168.137.1`。
- 测试（`demo/device/tests/sim/`）使用 C++ gtest runner，生产 `.c` 全部 C11 编译。
