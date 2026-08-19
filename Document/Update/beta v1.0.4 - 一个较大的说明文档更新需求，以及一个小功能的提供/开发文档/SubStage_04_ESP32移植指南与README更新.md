# SubStage_04：《硬件抽象层移植指南》与 README 更新

## SubStage: ESP32 移植指南文档 + README 层次说明更新
- 所属 Stage：Stage 2
- 依赖前置：SubStage_03（`demo/硬件抽象层接口规范.md` 必须已完成——本文档以它为接口语义唯一出处）
- 并行状态：需等待 SubStage_03 完成后方可开工
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

项目设备端代码最终要移植到 ESP32-C3（ESP-IDF）。SubStage_03 已产出《硬件抽象层接口规范》（接口语义的权威出处）。你的任务是编写**面向嵌入式开发者的移植指南**：一份零上下文文档，让一位只懂 ESP-IDF、不了解本仓库的嵌入式工程师读完后能独立完成"新建 ESP-IDF 工程 → 实现后端 → 跑通与上位机联调"。同时更新 `demo/README.md` 与 `demo/net_esp32c2/README.md` 的层次说明与文档索引。

目标平台口径（阶段一用户已确认）：**ESP32 系列通用 + 以 esp32-c3 为主移植目标，保留 esp32-c2 兼容说明**。

## 2. 输入材料（当前仓库状态）

| 材料 | 位置 | 用途 |
|------|------|------|
| 接口规范（依赖前置产物） | `demo/硬件抽象层接口规范.md` | 接口语义唯一出处；本文档引用而不复述逐条契约 |
| 抽象层接口定义 | `demo/include/net_abstraction.h` | 签名核对 |
| 嵌入式骨架（现有移植雏形） | `demo/net_esp32c2/esp32c2_backend.c` | 已有的 ESP-IDF 映射注释与 TODO；本指南在其经验上扩写 |
| 现有移植说明 | `demo/net_esp32c2/README.md`（71 行） | §3 已有 ESP-IDF 工程结构示例与 `app_main` 接入示例；§4 差异表（STA 异步事件、虚拟 IP 翻译不适用、NVS 键、mdns 服务类型、内存、单核事件循环、日志）——本指南沿用并扩写这些结论 |
| 设备业务层入口 | `demo/device/device_app.h`（`device_app_create/run`）、`demo/device/app/main.cpp`（宿主侧组装流程参照） | 移植后 `app_main` 的调用序列 |
| 参数与默认值 | `demo/include/params.h`、`demo/common/params.c`（`params_defaults`，`params_load` 支持 NULL 路径用默认值） | 嵌入式端不读 JSON 文件时的参数获取方式 |
| NVS 键约定 | `demo/device/device_provision.c`/`device_discovery.c`/`device_eventlog.c`（grep `nvs_` 键名：`wifi_creds`、`host_candidates`、`evlog`） | NVS 小节事实来源 |
| 协议常量 | `demo/include/protocol.h` | 端口 5935/5936、组播 224.0.2.1、mDNS `_tactile._tcp`、帧上限 1024 B |
| 流程文档 | 根目录 `ESP32-C2配网与连接保障流程.docx`（v3 H005） | 联调步骤与流程语义参照（1.2 节已注明流程同时覆盖 C2/C3） |

## 3. 交付物一：`demo/硬件抽象层移植指南.md`

零上下文文档，章节结构**必须**如下：

### 第 1 章 移植总览
- 移植需要做什么：**实现一个 `net_backend_t` 实例**（参照 `net_esp32c2/esp32c2_backend.c` 骨架）+ 编写 `app_main` 组装（参照 `net_esp32c2/README.md` §3 代码示例）+ 准备 ESP-IDF 工程。
- 不需要做什么：`demo/device/` 与 `demo/common/` 业务代码零修改（纯 C、零平台依赖、零 malloc 主路径）；协议与参数默认值内建。
- 目标芯片：esp32-c3（RISC-V，ESP-IDF ≥ 5.0）；esp32-c2 完全同构（`net_esp32c2` 目录名为历史命名，内容即 C2/C3 通用骨架）。

### 第 2 章 ESP-IDF 工程搭建
- 工程目录结构示例（沿用 `net_esp32c2/README.md` §3 的 `esp32c2_app/` 结构，说明需复制哪些源文件：`demo/device/*.c/.h`、`demo/common/*.c`、`demo/include/*.h`、`third_party/cjson/`、后端文件）。
- `idf_component_register` 的 SRCS/INCLUDE_DIRS 写法、`idf.py set-target esp32c3`、sdkconfig 要点（mdns 组件、NVS 分区、主任务栈大小建议）。

### 第 3 章 vtable 接口 → ESP-IDF API 映射表
表格形式，逐条给出 29 个接口的推荐 ESP-IDF 实现路径与注意事项。基线（需你核对骨架代码后确认或修正）：

| 分组 | 接口 | ESP-IDF 映射 | 注意事项 |
|------|------|--------------|----------|
| 生命周期 | `init`/`deinit` | `esp_event_loop_create_default`、`esp_wifi_init`、NVS flash init | `config_path` 参数在嵌入式下忽略（NULL 即可） |
| WiFi | `wifi_scan` | `esp_wifi_scan_start` + `esp_wifi_scan_get_ap_records` | `band_2g`：C3 仅 2.4G，恒为 1 |
| WiFi | `wifi_sta_connect` | `esp_wifi_set_config` + `esp_wifi_connect` + **事件组等待** | 同步语义转换：`WIFI_EVENT_STA_DISCONNECTED`（取 reason 映射）/ `IP_EVENT_STA_GOT_IP`；reason 映射 `WIFI_REASON_NO_AP_FOUND→201`、`AUTH_FAIL→202`（含 203/204 归类）、`HANDSHAKE_TIMEOUT→205` |
| WiFi | `wifi_ap_start`/`wifi_ap_stop` | `esp_wifi_set_mode(WIFI_MODE_AP)` + AP config（ssid/authmode=WPA2_PSK） | pin 参数在 ESP 侧无对应（PIN 为应用层 TCP 认证，骨架注释已说明） |
| WiFi | `wifi_get_rssi` | `esp_wifi_sta_get_ap_info` | AP 模式下无 STA 信息时返回 DEMO_ERR |
| WiFi | `wifi_get_ip`/`wifi_get_gateway`/`wifi_get_current_ssid` | `esp_netif_get_ip_info` / 已保存 STA config | `wifi_get_ip` 返回网络字节序 |
| TCP | `tcp_listen/accept/connect/send/recv/close` | lwIP BSD socket（`socket/bind/listen/accept/connect/send/recv/close`）+ `fcntl O_NONBLOCK` | recv/accept 非阻塞无数据返回 `DEMO_ERR_AGAIN`；connect 用 `select` 实现 timeout_ms |
| UDP | `udp_mcast_join/send/recv` | lwIP `setsockopt(IP_ADD_MEMBERSHIP)` + `sendto/recvfrom` | TTL=1 |
| mDNS | `mdns_register/unregister/resolve` | `mdns_service_add`/`mdns_service_remove`/`mdns_query_ptr` | 服务类型参数不含点号（`"_tactile","_tcp"`） |
| NVS | `nvs_get/set/erase` | `nvs_open("provision")` + `nvs_get_blob/set_blob/erase_key` | 命名空间 `provision`；键：`wifi_creds`/`host_candidates`/`evlog`（JSON 字节流）；键不存在时 `*len=0` 返回 DEMO_ERR |
| 系统 | `time_ms` | `esp_timer_get_time()/1000` | 必须单调（不能用 wall clock） |
| 系统 | `random` | `esp_random` | — |
| 注入 | `inject` | 直接返回 `DEMO_ERR` | 仅 sim 后端实现 |

### 第 4 章 关键实现细节
- WiFi 事件驱动 → 同步语义：事件组（`xEventGroupWaitBits`）完整示例代码（可直接采用骨架 `esp32c2_backend.c` 中已有注释思路扩写）。
- `app_main` 组装序列：`log_init` → `params_defaults`（嵌入式不读配置文件）→ `net_ctx_create(esp32_backend_get(), NULL)` → `device_app_create(&params, net, "<真实MAC>", seed)` → `device_app_run(app)`；设备 id 用 `esp_efuse_mac_get_default` 格式化为 `XX:XX:...`。
- 日志适配：`log.c` 在 ESP-IDF 下自动走无锁分支；可选改接 `ESP_LOGI`。
- 内存与任务模型：device 主循环阻塞式，直接跑在 `app_main` 或独立任务；cJSON 动态内存仅消息解析期（≤1KB）。
- 配网重置：长按按键 5 秒清 NVS 的接入建议（流程文档 5.3 节）。

### 第 5 章 与上位机联调步骤
1. 先用 sim 上位机（`demo/pc`，`--sim` 模式）+ 真机联调：PC 与开发板同一 WiFi；
2. 核对清单：热点名 `Modu_XXXX`（MAC 后 4 位大写十六进制）→ WPA2 密码/PIN 展示（串口打印）→ 配网 → mDNS/组播发现 → 会话心跳（`ping`/`pong`）→ 双向联调消息（界面发送 `app_data`）；
3. 常见失败点排查表（reason code、组播被路由器屏蔽时的候选列表兜底、防火墙）。

### 第 6 章 常见问题（FAQ）
至少覆盖：C2 与 C3 差异（无实质差异，API 同构）、为什么目录叫 net_esp32c2（历史命名）、单核下事件循环与 IDF 其他任务共存、NVS 磨损与写入防抖（业务层已内置，后端无需处理）、时间同步来源（上位机 `server_time`，非 NTP）。

## 4. 交付物二：README 更新

1. **`demo/README.md`**（129 行，只增不改既有内容）：
   - "目录结构"代码块下方补一段硬件抽象层说明："硬件抽象层 = `include/net_abstraction.h`（接口）+ `common/net_abstraction.c`（便捷封装）+ `net_sim`/`net_win`/`net_linux`/`net_esp32c2`（后端实现）；业务层通过 `net_ctx_t` 访问硬件，移植只需替换后端"。
   - 新增"文档索引"小节（如已有则追加条目）：`硬件抽象层接口规范.md`、`硬件抽象层移植指南.md`、既有 `设计文档.md`/`协议文档.md`。
2. **`demo/net_esp32c2/README.md`**：标题下方插入一行指引："本文档为骨架说明；完整的移植步骤请见 `demo/硬件抽象层移植指南.md`（以 esp32-c3 为主目标，兼容 C2）"。其余内容不动。

## 5. 验收标准

1. `demo/硬件抽象层移植指南.md` 存在且章节结构与本任务书第 3 节一致。
2. 映射表覆盖全部 29 个接口；接口名/签名/错误码与 `demo/硬件抽象层接口规范.md` 完全一致（抽查 5 处）。
3. 第 4 章 `app_main` 组装序列与 `net_esp32c2/README.md` §3 示例及 `device_app.h` 签名一致。
4. README 两处更新完成，既有内容未被破坏。
5. 不修改任何 `.c/.cpp/.h` 代码文件。

## 6. 禁止事项

- 不复述接口逐条契约（引用接口规范），避免双处维护。
- 不改动 `net_esp32c2/esp32c2_backend.c` 骨架代码（只读参考；发现骨架与文档结论矛盾时记录到完成报告上报，不自行修改）。
- 不改动协议常量口径（端口、命令名、组播地址等按 `protocol.h` 现状）。
- 不删除/重写 `net_esp32c2/README.md` 既有章节。

## 7. 输出与报告

- 交付文件：`demo/硬件抽象层移植指南.md`；修改 `demo/README.md`、`demo/net_esp32c2/README.md`。
- 完成报告内容：章节清单、映射表覆盖计数（29/29）、与接口规范一致性抽查结果、骨架矛盾上报（如有）。
