# ESP32-C2 后端骨架（移植指南）

> **完整移植步骤请见历史交付件 `Document/移植说明/MD/ESP32移植指南.md`，接口语义见
> `Document/移植说明/MD/ESP32移植方案.md`；本文档为骨架说明。beta v1.1 起 ESP32-C2 不在
> 开发与验收范围（本目录仅参与 `demo/device/tests/esp32` 的语法/接口完整性检查）。**

> **状态：骨架交付，未在真实硬件验证。** 本目录为纯 C 骨架，保持完整 `net_backend_t` 布局；宿主机构建仅允许通过 `demo/device/tests/esp32` 的语法/接口完整性检查目标编译检查，**不得将编译通过描述为硬件功能可用**。实机功能验收不在本版本通过条件内。

## 1. 目标芯片

- ESP32-C2（ESP8684）：RISC-V 单核 120MHz、576KB SRAM、无 FPU、WiFi 4 + BLE 5.0。
- 抽象层接口按 BSD socket 语义设计，lwIP 提供兼容 API，映射近乎 1:1。

## 2. 与宿主机构建的关系

- `esp32c2_backend.c` 为纯 C11；宿主机（Windows/Linux）上由 `tests/esp32` 的宿主检查目标做 C 语法与接口完整性检查，不链接 ESP-IDF 工具链。
- 本目录不参与 PC 端、设备 Qt 外壳或任何 C++ 目标的构建。
- ESP-IDF 工程入口见 `demo/device/firmware/esp32c2/`。

## 3. 逐接口实现状态（骨架基线）

下表为本版本交付时的接口状态；`DEMO_ERR` 均为占位返回值，未连接任何 ESP-IDF API：

| 接口 | 状态 | 真实实现要点 |
|---|---|---|
| `init` | 占位（返回 `DEMO_OK`，无操作） | `nvs_flash_init` / `esp_netif_init` / `esp_event_loop_create_default` / `esp_wifi_init` |
| `deinit` | 占位（无操作） | `esp_wifi_stop` / `esp_wifi_deinit` |
| `wifi_scan` | 占位（`DEMO_ERR`） | `esp_wifi_scan_start` / `esp_wifi_scan_get_ap_records` |
| `wifi_sta_connect` | 占位（`DEMO_ERR`，reason 置 `WIFI_REASON_OK`） | 事件驱动 + 事件组同步；reason 201/202/205 已对齐 |
| `wifi_sta_disconnect` | 占位（`DEMO_ERR`） | `esp_wifi_disconnect` |
| `wifi_ap_start` | 占位（`DEMO_ERR`） | `esp_wifi_set_mode(AP)` + WPA2-PSK + max_connection=1 |
| `wifi_ap_stop` | 占位（`DEMO_ERR`） | `esp_wifi_set_mode(WIFI_MODE_NULL)` |
| `wifi_get_rssi` | 占位（`DEMO_ERR`） | `esp_wifi_sta_get_ap_info` |
| `wifi_get_ip` | 占位（`DEMO_ERR`） | `esp_netif_get_ip_info`（网络字节序） |
| `wifi_get_current_ssid` | 占位（`DEMO_ERR`） | `esp_wifi_sta_get_ap_info` |
| `wifi_get_gateway` | 占位（`DEMO_ERR`，输出置 0） | `esp_netif_get_ip_info` 的 gw 字段 |
| `tcp_listen/accept/connect` | 占位（`DEMO_ERR`） | lwIP BSD socket，EAGAIN → `DEMO_ERR_AGAIN` |
| `sock_send/recv/close` | 占位（`DEMO_ERR` / 空操作） | lwIP send/recv/close |
| `udp_mcast_join/send/recv` | 占位（`DEMO_ERR`） | lwIP UDP + IP_ADD_MEMBERSHIP |
| `mdns_register/unregister/resolve` | 占位（`DEMO_ERR`） | IDF mdns 组件 |
| `nvs_get/set/erase` | 占位（`DEMO_ERR`） | 命名空间 `provision`；键 `wifi_creds` / `host_candidates` / `evlog` |
| `time_ms` / `random` | 占位（返回 0） | `esp_timer_get_time` / `esp_random` |
| `inject` | 明确不可用（`DEMO_ERR`） | 注入仅模拟后端可用 |

> **不宣称实机可用**：上表中所有"真实实现要点"均为移植指引，本版本未在任何 ESP32 硬件上验证。

## 4. 移植到 ESP-IDF 的步骤

工程骨架已建好：`demo/device/firmware/esp32c2/`（见其 README）。手动流程：

```bash
cd demo/device/firmware/esp32c2
idf.py set-target esp32c2
idf.py build
idf.py -p COMx flash monitor
```

## 5. 已知差异与注意事项

| 项 | 说明 |
|----|------|
| STA 连接异步性 | ESP-IDF WiFi 为事件驱动：`esp_wifi_connect()` 立即返回，结果经 `WIFI_EVENT_STA_DISCONNECTED`（带 reason）或 `IP_EVENT_STA_GOT_IP` 回调。`wifi_sta_connect` 的同步语义需用事件组/信号量等待（骨架注释已标注），reason code 数值已与 `wifi_reason_t` 对齐（201/202/205） |
| 虚拟 IP 翻译 | 真实环境无虚拟 IP；device 侧 `sess_host` 来自 mDNS/组播通告的真实 IP，`tcp_connect` 直连即可（无需翻译逻辑） |
| NVS 键 | 命名空间 `"provision"`；blob 键与 Demo 一致（`wifi_creds` / `host_candidates` / `evlog`），JSON 字节流存储 |
| mDNS | ESP-IDF mdns 组件在 sdkconfig 默认启用；`mdns_service_add` 服务类型参数不含点号（`"_tactile","_tcp"`） |
| 内存 | device 模块零 malloc（固定数组），cJSON 动态内存仅消息解析期（≤1KB）；576KB SRAM 充裕 |
| 单核事件循环 | `device_app_run` 为阻塞循环（内含 sleep），直接跑在 `app_main` 上下文即可；如需与 IDF 其他任务共存，可改为低优先级任务 |
| 日志 | `log.c` 的 Windows CRITICAL_SECTION 分支在 ESP-IDF 编译时自动走 pthread/无锁分支；建议改接 `ESP_LOGI`（可选） |
