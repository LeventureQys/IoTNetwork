# backends/linux：设备真实 Linux STA 后端（纯 C，beta v1.1 纯 STA 拓扑）

> **状态：纯 C，实例化；仅在 Linux 上编译真实实现。非 Linux 平台只提供
> `device_linux_backend_create` 的 NOT_SUPPORTED stub（`device_backend_linux_unavailable`），
> 不含任何热点/STA 源码。**

## 1. 目录与目标

```text
backends/linux/
├── CMakeLists.txt
├── README.md
├── device_linux_backend.h         # 纯 C factory 头（设计文档 §8.2.1 契约片段）
├── device_linux_backend.c         # 真实后端：linux_backend_t 装配 + net_backend_t vtable
├── device_linux_unavailable.c     # 非 Linux stub（仅 create，清零输出 + NOT_SUPPORTED）
├── linux_exec.h / linux_exec.c    # 安全子进程：argv + fork/exec，绝不拼 shell
├── linux_hotspot.h / .c           # hostapd+dnsmasq 热点（实例化；单实例锁/readiness/fail-closed/逆序 rollback）
├── linux_hotspot_config.h / .c    # 热点配置校验与地址规划（纯 C）
├── linux_hotspot_linux_ops.h / .c # 可注入 ops 表（run_argv/路由/pid 检查/root 等）
├── linux_wifi.h / .c              # STA（nmcli/ip/iw 全 argv 调用，密码不落日志）
├── linux_nvs.h / .c               # 文件 NVS（JSON+base64，与模拟后端同格式，原子替换）
├── linux_socket.h / .c            # POSIX TCP/UDP 组播/时钟/随机（非阻塞语义对齐 net_abstraction）
```

CMake 目标（供 SS06 聚合）：
- Linux：`device_backend_linux`（真实实现）
- 非 Linux：`device_backend_linux_unavailable`（stub）
两个目标都导出符号 `device_linux_backend_create`，签名同设计文档 §8.2.1。

## 2. 实例化改造说明（相对旧 demo/net_linux）

- 进程级全局 `g_cfg / g_plan / g_runtime / g_sta_iface / g_lock_fd / g_active` 全部移入
  `linux_hotspot_t` / `linux_wifi_t` / `linux_backend_t` 实例。
- `linux_hotspot_init/deinit` → `linux_hotspot_create/destroy`（ops 表作为 create 参数注入，
  取代旧的进程级 `set_ops_for_test`）。
- 保留语义：热点地址规划（`linux_hotspot_select_plan`）、单实例锁（flock）、readiness
  （hostapd pidfile/AP 模式、dnsmasq UDP 67 探测）、fail-closed、失败逆序 rollback。
- 外部命令全部 argv + fork/exec（`linux_exec_argv`）：nmcli/iw/ip/iptables/hostapd/dnsmasq/kill
  均不再经过 shell；SSID/密码只作为 argv 元素传递，日志不记录密码。

## 3. 后端 factory 契约

`device_linux_backend_create`（签名与 device/include/device_backend_factory.h 中 SS03
定义的跨 SubStage 契约一致，逐字复制自设计文档 §8.2.1）：

- options 可为 NULL；字符串仅 create 期间借用，后端内部复制。
- 进入函数清零 `*out_instance`；失败绝不返回部分实例；成功时
  `vtable/user/destroy_user` 均非空。
- 真实后端初始化失败（配置无效、非 root、锁冲突、缺依赖）直接返回错误并传播到
  facade，**绝不回退 sim**。
- 非 Linux 平台由 `device_linux_unavailable.c` 提供同签名 stub：清零输出并返回
  `DEVICE_ERR_NOT_SUPPORTED`。

## 4. 配置

beta v1.1 起后端为**纯 STA**（不再创建需要 root 的 hostapd/dnsmasq 热点实例）：

- `demo/device/config/device_linux.json`：标准 v1.1 设备字段（`pc_ap_ssid` 默认 `Modu_PC`、
  `pc_ap_password` 固定 `modu_leventure`、`pc_host_ip` 固定 `192.168.137.1`、
  `host_tcp_port` 固定 5935、`use_real_wifi_sta=1`）——与 sim 配置共用同一套
  `device_config_t` 加载/校验；文件缺失使用内置默认值并告警。
- Linux 后端附加字段（`linux_backend_config_load` 从同一 JSON 读取，均可省略）：
  - `enable`：默认 1；为 0 时后端拒绝启动（`DEVICE_ERR_BACKEND_UNAVAILABLE`）。
  - `sta_interface`：默认空（自动探测）；指定则作为 STA 网卡名传入
    `linux_wifi_create`。
  - `nvs_file`：文件 NVS 路径（默认由后端内部决定）。
- `demo/device/config/linux_hotspot.json`：**v1.1 后端不再加载**（热点实例已从
  `device_linux_backend_create` 移除）；该文件与 `linux_hotspot*` 源码仅供历史/独立
  模块测试保留，普通用户运行 `--backend linux` 不要求 root、不启动热点。

> 注意：旧版 README 提到的 `hotspot_config_path` 字段已随 v1.1 移除热点创建而删除，
> 任何文档/配置均不得再引用该字段。

## 5. 依赖与集成注意

- 需要 `log`（device/src/common，SS03）与 `cJSON`（device/third_party/cjson，SS03）的
  符号/头；CMake 中当前以"device 目录优先、根级目录回退"定位，SS08 删除根级目录前
  必须移除回退分支（见各 CMakeLists 注释）。
- 与 `device/include/device_backend_factory.h`（SS03）的包含约束：两个头不得出现在同一
  翻译单元（`device_result_t` 枚举重复定义）；集成时由 SS06/SS08 保证边界。
- mDNS 三接口（register/unregister/resolve）未实现，显式返回 `DEMO_ERR`（fail-closed）；
  `inject` 返回 `DEMO_ERR`（仅 sim 后端提供）。
- v1.1 后端 vtable 的 `wifi_ap_start/stop` 返回 `DEMO_ERR`（设备不创建热点；
  状态机不调用热点/`tcp_listen`/mDNS/UDP）。
- 真实 STA 验收（nmcli 连接 PC 热点、DHCP、直连 `192.168.137.1:5935`）需 Linux 环境 +
  无线网卡 + PC 热点，本仓库 Windows 开发机无法执行（见阶段三问题清单 CQ4）。

## 6. 测试

- `demo/device/tests/linux/`：fake ops 单测在普通 Linux 上自动运行（无需 root）；
  非 root/缺依赖/argv 直传/多实例锁/部分初始化销毁/禁止回退 sim 均有覆盖。
- `demo/device/tests/linux/device_linux_unavailable_check.c`：跨平台 stub 检查
  （Windows 可运行）。
- Linux 真实热点测试与 ESP-IDF 构建在本仓库 Windows 开发机上无法执行（见报告阻塞项）。
