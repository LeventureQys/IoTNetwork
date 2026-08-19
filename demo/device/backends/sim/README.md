# device/backends/sim - 设备模拟后端（纯 C11）

本目录是 SubStage 04 交付物：把旧 `demo/net_sim/`（C++ 单体）重写为
设备目录内的纯 C、实例化、可测试模拟后端。所有生产文件以 C11 编译，
无 `.cpp`、Qt、`std::`、class/new/delete、singleton 或进程级可变全局状态。

## 模块清单

| 文件 | 职责 |
|---|---|
| `sim_backend.c` | 上下文 `sim_backend_t`、29 项 `net_backend_t` vtable、工厂 `device_sim_backend_create` 与装配/逆序释放 |
| `sim_world.c` | 目标 WiFi / 虚拟 AP / RSSI / mDNS 注册表 / 组播屏蔽的显式实例状态（内部锁，线程安全） |
| `sim_socket.c` | TCP/UDP/组播非阻塞 socket 与平台错误映射（`WSAGetLastError`/`errno` → `DEMO_*`） |
| `sim_tcp_endpoint.c` | 模拟地址 → loopback 纯函数（真实模式不翻译） |
| `sim_nvs.c` | JSON blob 键值存储、严格 base64、同目录 tmp + flush + 原子替换 |
| `sim_ap_catalog.c` | 跨进程 AP catalog 发布/读取/删除（严格实现设计文档 8.5 契约） |
| `sim_mdns.c` | mDNS 注册/注销/超时轮询解析 |
| `sim_fault.c` | 故障注入 action 分派与发送失败计数 |
| `sim_random.c` | 实例化 xorshift32（seed=0 自动播种，非零 seed 可复现） |
| `sim_util.h` | 内部共享平台工具（时钟/休眠/PID/原子写/路径/IPv4），不构成模块 |
| `include/` | **临时**外部契约头（`device_host.h`、`device_backend_factory.h`、`net_abstraction.h`、`common.h`、`protocol.h`），SS03 向 `device/include/` 交付后由 SS06 删除并切换 |

## 工厂契约（设计文档 8.2.1）

`device_sim_backend_create(options, &out_instance, &error)`：

- 进入时清零 `out_instance`；成功时 `vtable/user/destroy_user` 均非空。
- 失败返回明确 `device_result_t`（INVALID_ARGUMENT / NO_MEMORY / BACKEND_INIT），
  `out_instance` 保持全零，无部分实例。
- options 字符串仅在调用期借用，内部复制。
- `sim_catalog_dir` 必须为显式绝对路径（契约 §5 / 设计文档 8.5），否则创建失败。
- `device_index` 范围 0～15；`provision_port==0` 时 `wifi_ap_start` 返回错误
  （无法发布合法 catalog）。
- `random_seed==0` 自动播种（等价旧 host 端随机），非零 seed 确定性复现。
- `nvs_file` 为空时回退旧默认 `run/dev<index>.nvs.json`；`host_virtual_ip`
  为空时回退 `192.168.1.50`（TCP 翻译沿用冻结的 `192.168.1.50` 虚拟地址，
  见下方"决策记录"）。
- 实例销毁走 `out_instance->destroy_user`：AP 运行中先注销 world 并删除
  catalog 正式文件与本进程 tmp；可接收 NULL（幂等）。struct 级幂等
  （全零实例）由 SS03 的 `device_backend_instance_destroy` 保证，本模块
  不重复实现。

## AP catalog 契约（设计文档 8.5，冻结）

- 文件名 `device-<device_index>.json`（0～15）；目录由 `sim_catalog_dir`
  显式传入，不从 NVS 目录或 CWD 推导。
- 写入：同目录 `device-<index>.json.tmp-<pid>` → `fflush` → 原子替换
  （Windows `MoveFileExA(REPLACE_EXISTING)` / POSIX `rename`）；失败清理 tmp。
- 停止 AP（`wifi_ap_stop`）与正常退出（`destroy_user`）删除正式文件及本进程 tmp。
- JSON 字段（类型严格，未知字段忽略，损坏/越界/非法 IP 记录被忽略）：
  `schema=1`、`device_index`、`device_id`、`ssid`、`bssid`、`logical_ip`、
  `loopback_host`、`provision_port`、`published_at_ms`（墙钟 ms）、`owner_pid`。
- 时效：记录超过 30 秒且 `owner_pid` 已不存在 → 忽略；无法判断 PID 时仅用 30 秒时效。
- 读取只使用正式文件（跳过 `.tmp-*`）；本模块 `remove` 仅设备侧调用，
  设备不删除其他进程的正式文件。

## 行为保持与决策记录

- vtable 保持 `net_abstraction.h` 29 项布局；init 恒成功（WSA 在 create 完成）。
- inject action 与旧实现同名：`wifi_disconnect/wifi_ok/wifi_auth_fail/wifi_auth_ok/
  wifi_ssid_mismatch/rssi_set/mcast_block/mcast_unblock/burst_send/sock_send_fail`；
  未知 action 返回 `DEMO_ERR`。
- NVS：非法 base64 失败（旧实现静默跳过非法字符）；缓冲不足返回错误并报所需长度，
  不截断（契约 §5）。文件格式兼容旧 `dev<n>.nvs.json`。
- TCP 翻译沿用冻结语义：连接模拟 AP 地址时经 catalog 取**索引最小**记录的真实端口
  （旧实现取共享 world 首个 AP，单设备一致）；连接 `192.168.1.50` 虚拟 host 时
  翻译到 `127.0.0.1:5935`（`PROTO_TCP_PORT`）。
- **冻结 schema 移除密码字段**：`wifi_sta_connect` 对 catalog 中存在的 SSID
  视为可达（旧实现可校验 catalog 密码，新 schema 无此字段；设备业务流不会连接
  其他设备 AP，影响面为空）。catalog 不含 password/pin（8.5 节 schema 冻结）。
- `options.host_virtual_ip` 被复制保存但**不参与** TCP 翻译（翻译沿用旧实现
  固定的 `192.168.1.50`），供上层/诊断使用。
- 测试（`demo/device/tests/sim/`）使用 C++ gtest runner，生产 `.c` 全部 C11 编译。
