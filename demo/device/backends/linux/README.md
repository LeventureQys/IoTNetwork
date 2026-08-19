# backends/linux：设备真实 Linux 热点 / STA 后端（纯 C）

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

- `demo/device/config/device_linux.json`：Linux 专用字段（`enable` / `sta_interface` /
  `hotspot_config_path` / `nvs_file`），协议字段与 golden 一致；文件缺失使用内置默认值并告警。
- `demo/device/config/linux_hotspot.json`：迁移自 `demo/config/linux_hotspot.json`，
  内容与字段语义不变。

## 5. 依赖与集成注意

- 需要 `log`（device/src/common，SS03）与 `cJSON`（device/third_party/cjson，SS03）的
  符号/头；CMake 中当前以"device 目录优先、根级目录回退"定位，SS08 删除根级目录前
  必须移除回退分支（见各 CMakeLists 注释）。
- 与 `device/include/device_backend_factory.h`（SS03）的包含约束：两个头不得出现在同一
  翻译单元（`device_result_t` 枚举重复定义）；集成时由 SS06/SS08 保证边界。
- mDNS 三接口（register/unregister/resolve）未实现，显式返回 `DEMO_ERR`（fail-closed）；
  `inject` 返回 `DEMO_ERR`（仅 sim 后端提供）。
- 真实热点验收（root + hostapd/dnsmasq + 支持 AP 的网卡）单独执行，不属于普通 CI。

## 6. 测试

- `demo/device/tests/linux/`：fake ops 单测在普通 Linux 上自动运行（无需 root）；
  非 root/缺依赖/argv 直传/多实例锁/部分初始化销毁/禁止回退 sim 均有覆盖。
- `demo/device/tests/linux/device_linux_unavailable_check.c`：跨平台 stub 检查
  （Windows 可运行）。
- Linux 真实热点测试与 ESP-IDF 构建在本仓库 Windows 开发机上无法执行（见报告阻塞项）。
