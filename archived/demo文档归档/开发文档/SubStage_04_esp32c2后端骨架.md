# SubStage_04：esp32c2 后端骨架

## SubStage: esp32c2 后端骨架
- 所属 Stage：Stage 2
- 依赖前置：SubStage_02（net_abstraction.h 定稿）
- 并行状态：需等待 SubStage_02；可与 SubStage_03 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

交付网络抽象层的 ESP32-C2（ESP8684，RISC-V 单核 120MHz）真实后端**骨架**：每个抽象接口对应 ESP-IDF API 的映射实现（纯 C，仅含头文件引用与函数体占位/最小实现），附 `component` 目录结构与移植说明。**不参与宿主机构建、不编译、不验收运行**——作为"抽象层为真实芯片留好路径"的交付物与移植指南。

## 2. 当前代码状态

- 已有：SubStage_01、SubStage_02。
- 依据：`设计文档.md` 2.2、4.6；阶段二问题清单 Q12（交付形态）、Q-D4（reason code 对齐）。

## 3. 交付物（全部新建，纯 C99）

```
demo/net_esp32c2/include/esp32c2_impl.h     # 后端实例结构（net_ctx user 数据）与内部辅助
demo/net_esp32c2/esp32c2_backend.c          # vtable 全成员实现（骨架）
demo/net_esp32c2/README.md                 # 移植说明（含 component 结构、IDF 版本、接线/编译步骤）
```

## 4. 接口契约与 ESP-IDF 映射表（esp32c2_backend.c 按此实现）

| 抽象接口 | ESP-IDF API 映射 | 骨架实现要求 |
|----------|------------------|--------------|
| `init` | `nvs_flash_init()`；`esp_netif_init()`；`esp_event_loop_create_default()`；`esp_wifi_init()` | 完整调用序列 + 错误返回 |
| `deinit` | `esp_wifi_stop()`；`esp_wifi_deinit()` | 完整调用 |
| `wifi_scan` | `esp_wifi_scan_start()` + `esp_wifi_scan_get_ap_records()` | 完整实现（含过滤/截断逻辑） |
| `wifi_sta_connect` | `esp_wifi_set_mode(WIFI_MODE_STA)` + `esp_wifi_set_config()` + `esp_wifi_connect()`；reason 从 `WIFI_EVENT_STA_DISCONNECTED` 事件 `reason` 字段映射（`wifi_reason_t` 与 `WIFI_REASON_*` 数值已对齐：201/202/205） | 事件驱动：骨架中以注释写明事件注册方式，函数体返回 DEMO_ERR（占位） |
| `wifi_sta_disconnect` | `esp_wifi_disconnect()` | 占位 |
| `wifi_ap_start` | `esp_wifi_set_mode(WIFI_MODE_AP)` + `esp_wifi_set_config()`（`wifi_config_t.ap`，SSID `Modu_XXXX`、`authmode=WIFI_AUTH_WPA2_PSK`、password） | 完整调用序列 |
| `wifi_ap_stop` | `esp_wifi_set_mode(WIFI_MODE_NULL)` | 占位 |
| `wifi_get_rssi` | `esp_wifi_sta_get_ap_info()` → `rssi` | 完整实现 |
| `wifi_get_ip` | `esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))` → `ip4_addr` | 完整实现（返回网络字节序） |
| `tcp_listen` | lwIP `socket/listen/bind`（`IPADDR_ANY`）| 完整实现（非阻塞：`fcntl(O_NONBLOCK)` 或 `lwip_setsockopt`） |
| `tcp_accept` | lwIP `accept` | 完整实现 |
| `tcp_connect` | lwIP `connect`（非阻塞 + `select` 超时） | 完整实现 |
| `sock_send/recv/close` | lwIP `send/recv/close`；EAGAIN → DEMO_ERR_AGAIN | 完整实现 |
| `udp_mcast_join` | lwIP `socket/bind(port)` + `IP_ADD_MEMBERSHIP` | 完整实现 |
| `udp_send` | lwIP `sendto`（组播，`IP_MULTICAST_TTL=1`） | 完整实现 |
| `udp_recv` | lwIP `recvfrom` | 完整实现 |
| `mdns_register` | `mdns_init()` + `mdns_hostname_set()` + `mdns_instance_name_set()` + `mdns_service_add()` + `mdns_service_txt_item_set()` | 完整实现 |
| `mdns_unregister` | `mdns_service_remove()` | 完整实现 |
| `mdns_resolve` | `mdns_query_a()`（轮询直到超时） | 完整实现 |
| `nvs_get/set/erase` | `nvs_open(NVS_NAMESPACE)` + `nvs_get_blob/set_blob/erase_key` | 完整实现（命名空间常量 `"provision"`） |
| `time_ms` | `esp_timer_get_time()/1000` | 完整实现 |
| `random` | `esp_random()` | 完整实现 |
| `inject` | 无（模拟专用） | 返回 DEMO_ERR，注释说明 |

实现约束：
- 全部函数用 `ESP_LOGI`/`ESP_LOGE` 打日志；头文件包含 `<esp_wifi.h>`、`<esp_netif.h>`、`<esp_event.h>`、`<nvs_flash.h>`、`<mdns.h>`、`<esp_timer.h>`、`<esp_random.h>`、`<lwip/sockets.h>` 等（按需）。
- 后端表实例：`static const net_backend_t g_esp32c2_backend = { ... }`，导出 `const net_backend_t *esp32c2_backend_get(void);`。
- 移植说明 README.md 必须包含：component 目录结构（`main/` + `CMakeLists.txt` + `idf_component.yml` 声明 mdns 依赖）、`idf.py build/flash/monitor` 步骤、ESP32-C2 目标设置（`idf.py set-target esp32c2`）、与宿主机构建的关系（本目录不参与）、已知差异（esp_wifi 事件驱动的 STA 连接结果需在事件回调中完成态传递——骨架的占位函数注释中写明建议的事件处理方式）。

## 5. 验收标准

1. 全部文件为纯 C99（无 C++），可被 C 编译器语法检查通过（宿主机构建中**不编译**该目录；验收以代码审查 + 语法检查为准：用 MSVC `/TC /Zs`（仅语法）对 esp32c2_backend.c 做一次语法检查，允许因缺少 ESP-IDF 头文件而失败——若语法检查因缺头失败，则以人工审查为准并记录）。
2. 映射表中每行接口在 .c 中有对应实现或注释占位，且 ESP-IDF API 名称拼写正确（人工核对 ESP-IDF v5.x API）。
3. README.md 包含完整移植步骤与差异说明。
4. 根 CMakeLists.txt 提供选项 `DEMO_BUILD_ESP32C2_BACKEND`（默认 OFF），开启时仅加入 include 路径不编译（占位），注释说明。
5. 本任务书**不要求编译通过、不要求单测**（无硬件与 IDF 环境）。

## 6. 禁止事项

- 不实现任何 device 业务逻辑（状态机/心跳等在 device/ 目录，与后端无关）。
- 不修改 net_abstraction.h 契约。
- 不伪造"已硬件验证"的表述；README 中必须显著标注"未在真实硬件验证"。
- 不引入 C++。
- 不使用模拟专用接口（inject 除外，必须返回 DEMO_ERR）。

## 7. 依赖前置

- 等待 SubStage_02 定稿后开工；与 SubStage_03 并行。
- 本任务书产出物不参与 Stage 3~6 的宿主机构建与测试，仅作为交付文档审查。
