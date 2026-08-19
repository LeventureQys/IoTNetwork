# SubStage_01：工程骨架与基础设施

## SubStage: 工程骨架与基础设施
- 所属 Stage：Stage 1
- 依赖前置：无依赖
- 并行状态：可立即开工
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

在 `Document/Update/Research/嵌入式设备组网配网流程/demo/` 下搭建独立 CMake 工程骨架，vendor 第三方依赖（cJSON、googletest），实现公共基础设施：日志、长度前缀帧编解码、参数配置、协议常量。产出后工程可配置、可构建、可跑基础设施单测。

## 2. 当前代码状态

- 目录 `demo/` 已存在，含文档：`问题清单.md`、`阶段二问题清单.md`、`协议文档.md`、`设计文档.md`。**无任何代码**。
- 依据：`设计文档.md` 第 3 节（目录结构）、4.1~4.5（接口契约）、5（构建）、7（测试策略）。
- 网络：cJSON v1.7.18 可从 `https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.{c,h}` 下载；googletest v1.15.2 从 `https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.15.2` 下载（github.com 归档页不可达，勿用 FetchContent 默认 URL）。

## 3. 交付物（全部新建）

```
demo/CMakeLists.txt
demo/tests/CMakeLists.txt
demo/include/common.h
demo/include/log.h
demo/common/log.c
demo/include/protocol.h
demo/include/frame.h
demo/common/frame.c
demo/include/params.h
demo/common/params.c
demo/third_party/cjson/{cJSON.c,cJSON.h,LICENSE}
demo/third_party/googletest/（源码解压，含 LICENSE）
demo/config/demo_config_default.json
demo/config/demo_config.json
```

## 4. 接口契约（与本 SubStage 相关）

### 4.1 common.h（include/common.h，纯 C99）

```c
#ifndef DEMO_COMMON_H
#define DEMO_COMMON_H
#include <stdint.h>
#include <stddef.h>

#define DEMO_OK         0
#define DEMO_ERR       -1
#define DEMO_ERR_NOMEM -2
#define DEMO_ERR_INVAL -3
#define DEMO_ERR_AGAIN -4
#define DEMO_ERR_TIMEOUT -5

typedef enum { LOG_TRACE=0, LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } log_level_t;
#endif
```

### 4.2 log.h（include/log.h）与 common/log.c

```c
void log_init(int level);
void log_set_level(int level);
void log_msg(int level, const char *module, const char *fmt, ...);
```

- 输出格式：`[YYYY-MM-DD HH:MM:SS.mmm][LEVEL][MODULE] message`，毫秒时间戳。
- 线程安全：Windows CRITICAL_SECTION 保护（内部实现，仅 Windows，工程只构建 Windows）。
- `log_msg` 为空 fmt 时不输出；level < 当前级别时直接返回（不执行格式化）。
- 实现细节：模块前缀与消息在同一行；ERROR 输出到 stderr，其余 stdout（可合并，保持简单）。

### 4.3 protocol.h（include/protocol.h，纯 C99）

```c
#define PROTO_VERSION         1
#define PROTO_MSG_MAX_LEN     1024
#define PROTO_FRAME_HEAD_LEN  2
#define PROTO_TCP_PORT        5935
#define PROTO_MCAST_GROUP     "224.0.2.1"
#define PROTO_MCAST_PORT      5936
#define PROTO_MDNS_TYPE       "_tactile._tcp"
#define PROTO_MDNS_INSTANCE   "host"
#define PROTO_HOST_MAX_CONN   16
#define PROTO_WIFI_CRED_MAX   2
#define PROTO_CANDIDATE_MAX   2
#define PROTO_AP_IP           "192.168.1.1"
#define PROTO_SESSION_ID_LEN  8

#define CMD_AUTH          "auth"
#define CMD_AUTH_RESULT   "auth_result"
#define CMD_WIFI_CONFIG   "wifi_config"
#define CMD_WIFI_RESULT   "wifi_result"
#define CMD_CLOSE_AP      "close_ap"
#define CMD_HOST_ANNOUNCE "host_announce"
#define CMD_HOST_BYE      "host_bye"
#define CMD_DEVICE_HELLO  "device_hello"
#define CMD_HOST_ACK      "host_ack"
#define CMD_PING          "ping"
#define CMD_PONG          "pong"
#define CMD_DIAG_QUERY    "diag_query"
#define CMD_DIAG_REPORT   "diag_report"
#define CMD_APP_DATA      "app_data"    /* 通用业务占位（协议文档 2.5），Demo 未启用业务负载 */

typedef enum {
    WIFI_REASON_OK             = 0,
    WIFI_REASON_NO_AP_FOUND    = 201,
    WIFI_REASON_AUTH_FAIL      = 202,
    WIFI_REASON_HANDSHAKE_TIMEOUT = 205,
    WIFI_REASON_5G_BAND        = 500
} wifi_reason_t;

typedef enum {
    DEV_STATE_BOOT = 0, DEV_STATE_STA_JOIN, DEV_STATE_AP_PROVISION,
    DEV_STATE_DISCOVERY, DEV_STATE_CONNECT, DEV_STATE_SESSION, DEV_STATE_HEAL,
    DEV_STATE_COUNT
} device_state_t;
const char *device_state_str(device_state_t s);
```

- `device_state_str` 返回常量字符串，映射：`boot`/`sta_join`/`ap_provision`/`discovery`/`connect`/`session`/`heal`；越界返回 `"unknown"`。
- 实现于 common/protocol.c（新增该文件）。
- 协议文档 v1.1 通用消息占位说明：`CMD_APP_DATA` 与 `vendor_*` 前缀不新增常量（动态命令名，接收方按协议文档 2.6 处理）。

### 4.4 frame.h（include/frame.h）与 common/frame.c

```c
int frame_wrap(const uint8_t *payload, int len, uint8_t *out, int out_cap);
int frame_parse(const uint8_t *data, int data_len, int *out_off, int *out_len, int *consumed);
```

- `frame_wrap`：payload 为 NULL 且 len==0 允许（空 JSON 负载不存在，本 Demo 无空负载）；len<0 或 len>PROTO_MSG_MAX_LEN 返回 DEMO_ERR；out_cap < len+2 返回 DEMO_ERR；输出 `[len_hi][len_lo][payload...]`（大端）；返回 len+2。
- `frame_parse`：流式解析，详见设计文档 4.4；`consumed` 必填非 NULL；畸形判定：首字节为 0 且长度<2 时返回 0（需更多数据）；完整长度读出后 `len==0 || len>PROTO_MSG_MAX_LEN` → DEMO_ERR 且 consumed=2；数据不足 → 0；`out_off` 指向 payload 在 data 中的偏移、`out_len` 为 payload 长度（已包含 head 的 consumed 扣除）。
- 注意：本函数在 demo 中是**纯状态机**（调用方保留剩余字节），不维护内部状态。

### 4.5 params.h（include/params.h）与 common/params.c

结构体字段与默认值严格按 `设计文档.md` 4.5 节复制实现（demo_params_t 全字段 + params_load 语义）。JSON 键名 = 结构体字段名（snake_case 一致）。

- `params_load(p, path)`：path 为 NULL 或文件不存在 → 仅默认值，返回 DEMO_OK；文件存在 → 用 cJSON 解析，`cJSON_IsNumber` 的数字字段覆盖、`cJSON_IsString` 的字符串字段覆盖（超长截断，不报错）；返回 DEMO_OK；JSON 语法错误 → DEMO_ERR（调用方日志）。
- `demo_params_t` 增加两个字段（设计文档补充）：`char scenario_path[128];`（默认空）、`char config_tag[32];`（默认空，仅日志展示）。

### 4.6 config 样例

- `demo_config_default.json`：字段与设计文档 4.5 默认值完全一致（供阅读对照）。
- `demo_config.json`：演示压缩配置：power_on_jitter_max_ms=1000、provision_auth_timeout_ms=3000、provision_wifi_cfg_timeout_ms=8000、provision_ap_idle_timeout_ms=15000、provision_confirm_window_ms=12000、heartbeat_interval_ms=2000、rssi_sample_interval_ms=1000、rssi_bad_duration_ms=6000、watchdog_state_timeout_ms=30000、discovery_fast_window_ms=8000、discovery_candidate_timeout_ms=6000、reconnect_backoff_cap_ms=5000、busy_backoff_ms=6000、duration_s=45；device_count=1、target_ssid="TactileFactory-2.4G"、target_password="securepass123"；其余保持默认。JSON 中只列出现有字段。

### 4.7 CMakeLists.txt（根）

- 工程名 `TactileSenseDemo`，C++17；`cmake_minimum_required(VERSION 3.20)`。
- MSVC：`/W4 /WX`（警告即错误）；C 文件（common/*.c）用 `/TC` 或通过 `set_source_files_properties(... PROPERTIES LANGUAGE C)` 强制 C 编译，标准 C99（`/std:c11`）。
- 目标 `provision_demo_core`（静态库）：common/ + include/（后续 Stage 追加 device/、host/、net_sim/ 源文件时由对应任务书修改本文件，本任务书先只含 common 与 include）。
- 目标 `provision_demo`（可执行）：app/main.cpp（Stage 5 才有，本任务书先建空壳 main 返回 0，后续替换）。
- 目标 `provision_demo_tests`（可执行）：tests/ 全部 test_*.cpp + 链接 provision_demo_core + googletest（`add_subdirectory(third_party/googletest)` 用 `gtest_main`）。
- cJSON：`third_party/cjson/cJSON.c` 加入 core 编译（LANGUAGE C），include 路径 `third_party/cjson`。
- 可执行文件输出目录统一 `build/bin`（`CMAKE_RUNTIME_OUTPUT_DIRECTORY`）。
- `tests/CMakeLists.txt`：被根 add_subdirectory(tests) 引用；本任务书在 tests/ 下只放 `test_frame.cpp`、`test_params.cpp`、`test_protocol.cpp`（其余测试后续 SubStage 追加）。

### 4.8 tests（本任务书范围）

- `test_frame.cpp`：wrap 正常（payload "{}" 长度 2 → out 4 字节 [0][2][{][}]）；wrap 超长（1025）→ DEMO_ERR；wrap 容量不足 → DEMO_ERR；parse 单帧完整；parse 两帧粘包（consumed 与 out 正确）；parse 拆包（分两次喂）；parse 长度 0 → DEMO_ERR；parse 长度 1025 → DEMO_ERR；parse 空输入 → 0。
- `test_params.cpp`：默认值（不传 path）；JSON 覆盖数字/字符串；缺键保持默认；非法 JSON → DEMO_ERR；越界字符串截断。
- `test_protocol.cpp`：device_state_str 全映射 + 越界；13 种标准命令名常量与协议文档 2.4 命令空间一致；`CMD_APP_DATA` 存在。

## 5. 验收标准

1. `cmake -G "Visual Studio 17 2022" -A x64 -S . -B build` 成功；`cmake --build build --config Debug` 成功，零警告（/W4 /WX）。
2. `provision_demo_tests.exe` 三个测试文件全部通过。
3. `provision_demo.exe` 启动退出码 0。
4. `demo_config.json` 可被 params_load 正确解析（测试覆盖）。
5. cJSON/googletest LICENSE 文件随 vendor 保留。

## 6. 禁止事项

- 不实现任何网络、设备、host 逻辑（本任务书只做骨架与公共层）。
- 不修改 `设计文档.md` 中已定义的接口签名；如需调整，报告 MainAgent 自决（自决区 Q-D 记录）。
- 不使用 `std::thread`/socket 等（公共层无并发与网络）。
- 不创建 README.md（Stage 5 交付）。
- 不允许在 C 文件中使用 C++ 语法；在 .c 中使用 cJSON 需包含 "cJSON.h"。

## 7. 依赖前置

- 无。可立即开工。
- 依赖下载 URL 已实测可达（见第 2 节）；下载失败时报告 MainAgent（可能需切换镜像）。
