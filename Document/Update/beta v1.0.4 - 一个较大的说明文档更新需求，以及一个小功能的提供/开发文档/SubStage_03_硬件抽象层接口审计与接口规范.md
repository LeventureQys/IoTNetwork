# SubStage_03：硬件抽象层接口审计与《硬件抽象层接口规范》

## SubStage: 硬件抽象层接口审计 + 接口规范文档
- 所属 Stage：Stage 1
- 依赖前置：无依赖（独立可执行，可立即开工）
- 并行状态：可与 SubStage_01 / SubStage_02 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

本项目最终要把设备端代码从 Linux（rk3588 OrangePi）验证环境移植到 ESP32-C3（ESP-IDF）。项目已有一套完整的硬件抽象：`demo/include/net_abstraction.h` 定义 `net_backend_t` vtable（29 个函数指针：生命周期 2 + WiFi 介质 9 + TCP 6 + UDP 组播 3 + mDNS 3 + NVS 3 + 系统 2 + 注入 1），`demo/common/net_abstraction.c` 提供 `net_ctx_t` 便捷封装，`net_sim`/`net_win`/`net_linux`/`net_esp32c2` 为四个后端实现；业务层（`demo/device/`、`demo/common/` 其余模块、`demo/pc/core/`）全部通过该抽象访问硬件。

你的任务：

1. **接口完备性审计**：对照流程文档逐点核对抽象层接口是否覆盖全部流程需求，产出审计结论。
2. **编写《硬件抽象层接口规范》**：一份零上下文、可直接交给嵌入式开发者与后端实现者的接口规范文档。

本 SubStage **纯文档工作，不修改任何代码文件**。

## 2. 输入材料（当前仓库状态）

| 材料 | 位置 | 用途 |
|------|------|------|
| 抽象层接口定义 | `demo/include/net_abstraction.h`（98 行） | 29 个 vtable 函数指针 + `net_addr_t`/`net_ap_info_t`/`net_mdns_service_t` 数据结构 + `net_xxx` 便捷封装声明 |
| 便捷封装实现 | `demo/common/net_abstraction.c` | NULL 安全转发语义 |
| 行为参照后端 | `demo/net_sim/sim_backend.cpp`（+ `sim_world.cpp`） | 接口语义的权威行为参照（测试全部基于它） |
| 嵌入式骨架 | `demo/net_esp32c2/esp32c2_backend.c` + 同目录 `README.md` | 移植已知差异（STA 事件驱动、虚拟 IP、NVS 键、mdns 组件等） |
| 流程文档 | 根目录 `ESP32-C2配网与连接保障流程.docx`（v3 增强版 H005） | 审计基线：五阶段流程每个功能点依赖哪些硬件能力。文档要点：五阶段（启动自检/SoftAP 配网/服务发现三级降级/会话保活/异常自愈）；reason code 201/202/205 分类；心跳 ping/pong；组播 224.0.2.1:5936；mDNS `_tactile._tcp`；NVS 持久化（凭据/候选列表/事件日志）；LED 状态指示（模拟环境以日志代替） |
| 业务层调用方 | `demo/device/*.c`（provision/discovery/session/heal/app） | 第 3 章"业务层调用时序"的事实来源（用 Grep 搜索 `net_` 前缀调用即可枚举） |
| 错误码 | `demo/include/common.h` | `DEMO_OK`/`DEMO_ERR`/`DEMO_ERR_AGAIN` 定义 |
| 协议常量 | `demo/include/protocol.h` | 端口、组播地址、消息上限等（引用，不修改） |

## 3. 审计方法（先审计，后写文档）

1. 从 `demo/device/` 与 `demo/pc/core/` 中 grep 全部 `net_` 前缀调用，得到"业务层实际使用的接口清单"。
2. 对照流程文档五阶段功能点，逐点确认：该功能依赖的硬件能力是否由 vtable 接口提供？由哪个接口提供？
3. 审计基线表（写入文档附录，需你实际核对后确认或修正）：

| 流程点 | 依赖接口 |
|--------|----------|
| WiFi 扫描（上位机找 Modu_XXXX 热点 / 频段判断） | `wifi_scan`（`net_ap_info_t` 含 `band_2g`） |
| STA 连接与 reason code 分类（201/202/205） | `wifi_sta_connect`（`wifi_reason_t *reason` 出参）、`wifi_sta_disconnect` |
| SoftAP 配网热点启停 | `wifi_ap_start(ssid, pass, pin)`、`wifi_ap_stop` |
| IP 获取（AP 虚拟 IP / STA IP / 网关） | `wifi_get_ip`、`wifi_get_gateway`、`wifi_get_current_ssid` |
| RSSI 监测（5s 采样、-75dBm 阈值） | `wifi_get_rssi` |
| TCP 配网服务器（设备 AP 模式 5935） | `tcp_listen`/`tcp_accept`/`sock_send`/`sock_recv`/`sock_close` |
| TCP 业务连接（设备连上位机） | `tcp_connect`（带超时）+ 上述收发 |
| mDNS 主发现 | `mdns_register`/`mdns_unregister`/`mdns_resolve` |
| UDP 组播兜底 | `udp_mcast_join`/`udp_send`/`udp_recv` |
| NVS 持久化（凭据集/候选列表/事件日志） | `nvs_get`/`nvs_set`/`nvs_erase`（blob 键值） |
| 上电错峰/退避抖动/心跳计时 | `time_ms`（单调毫秒）、`random` |
| 故障注入（仅 sim） | `inject`（其余后端返回 DEMO_ERR） |

4. 已知"不适用项"（直接标注，不算缺口）：LED 状态指示（业务层以日志模拟，无接口依赖）；NTP（可选增强，未实现）。
5. 若审计发现真实缺口（某流程点无接口支撑）：**不要自行改代码**，记录到完成报告并停止，上报 MainAgent 决策。

## 4. 交付物：`demo/硬件抽象层接口规范.md`

零上下文文档（读者是无对话历史、不熟悉本仓库的嵌入式开发者），章节结构**必须**如下：

### 第 1 章 层次边界与移植结论
- 架构图（UI 层 / 业务层 / 硬件抽象层 / 后端实现，参照设计文档 §2 的文字图）。
- 核心结论："移植 = 实现一个新的 `net_backend_t` 实例；业务层（`demo/device/`、`demo/common/`）零修改"。
- 目标平台口径：**ESP32 系列通用 + 以 esp32-c3 为主移植目标，兼容 esp32-c2**（ESP-IDF 下两芯片 WiFi/lwIP/mDNS/NVS API 兼容；`net_esp32c2` 目录名为历史命名）。

### 第 2 章 接口契约（逐条）
按 vtable 分组逐条写全 29 个接口，每条包含：签名、参数语义（含入参/出参约定，如 `count 入=容量 出=数量`）、返回值（`DEMO_OK`/`DEMO_ERR`/`DEMO_ERR_AGAIN` 各自含义）、阻塞性（阻塞/非阻塞/超时参数）、调用频率与线程要求、NULL 安全行为（引用 `net_abstraction.c` 便捷封装语义）。分组：生命周期（init/deinit）、WiFi 介质（9 个）、TCP（6 个）、UDP 组播（3 个）、mDNS（3 个）、NVS（3 个）、系统（time_ms/random）、注入（inject）。
数据结构小节：`net_addr_t`（ip/port 均网络字节序）、`net_ap_info_t`（ssid 33B 含结尾符、band_2g 语义）、`net_mdns_service_t`（type 不含点号的写法见 esp32c2 README 差异表）。
错误码小节：引用 `common.h` 定义，说明 `DEMO_ERR_AGAIN` 的非阻塞 recv/accept 语义。

### 第 3 章 业务层调用时序
按设备状态机七状态（boot/sta_join/ap_provision/discovery/connect/session/heal）列出每个状态调用的后端接口、顺序、周期（如 session 态：`sock_recv` 每轮循环、`sock_send` 心跳 10s、`wifi_get_rssi` 5s）。事实来源：实际代码 grep，不得凭空编写。

### 第 4 章 后端实现者检查清单
- 必须实现的接口清单与最小可用集（哪些接口可以返回 DEMO_ERR 而不破坏主流程——例如 `inject`、`mdns_*` 有组播兜底时的降级行为）；
- 行为对齐要求：以 `net_sim` 后端为行为参照物，`demo/tests/` 全量测试通过即视为语义对齐；
- 常见实现陷阱（引用 `net_esp32c2/README.md` §4 差异表：STA 异步事件、虚拟 IP 不适用、mdns 服务类型格式等）。

### 第 5 章 可选扩展接口（预留）
- 说明本版不含 LED/GPIO 接口的原因（业务层无调用点）；
- 给出推荐扩展示例：`int (*led_set)(void *user, int state);` 及接入位置建议（业务层 `device_set_state` 处），注明扩展需同步修改 `net_backend_t` 与所有后端占位。

### 附录 A 接口完备性审计结论表
第 3 节审计基线表的核对结果（流程点 → 接口 → 结论"已覆盖/不适用/缺口"），附 grep 证据（业务层调用位置示例）。

## 5. 验收标准

1. 文档存在于 `demo/硬件抽象层接口规范.md`，章节结构与本任务书第 4 节一致。
2. 29 个接口逐条覆盖，无遗漏；每条含签名、参数、返回值、阻塞性四要素。
3. 第 3 章调用时序可在代码中找到对应调用点（抽查 3 处无虚构）。
4. 审计结论表完整，缺口（如有）已上报。
5. 不修改任何代码文件。

## 6. 禁止事项

- 不修改任何 `.c/.cpp/.h/CMakeLists.txt` 文件。
- 不重写或移动 `net_esp32c2/README.md`（SubStage_04 会加一行指引，与你无关）。
- 不在文档中改动协议常量口径（端口 5935/5936、组播 224.0.2.1、消息上限 1024 B 等一律按 `protocol.h` 现状书写）。
- 不引入"应该新增接口"的结论而不走上报流程。

## 7. 输出与报告

- 交付文件：`demo/硬件抽象层接口规范.md`。
- 完成报告内容：审计结论摘要（覆盖/不适用/缺口计数）、缺口上报警告（如有）、文档章节清单。
