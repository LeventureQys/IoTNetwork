# SubStage SS1.1：跨端协议与配置契约

## SubStage: 跨端协议与配置契约
- 所属 Stage：Stage 1 - 冻结跨端契约
- 依赖前置：阶段一最终问题清单已确认
- 并行状态：需等待；本任务必须先完成，Stage 2 所有任务才能开工
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

beta v1.1 将旧的设备 SoftAP 配网和服务发现流程替换为 PC 热点 + Linux 设备固定地址直连。本任务只冻结双方共享常量、配置字段、设备状态和 JSON contract，不实现 Windows 热点、状态机或 UI。

## 2. 当前代码状态

- PC 协议：`demo/pc/include/pc/protocol.h:9`。
- 设备协议：`demo/device/include/protocol.h:9`。
- PC 配置：`demo/pc/include/pc/params.h:9`、`demo/pc/common/params.c:7`。
- 设备配置：`demo/device/include/device_config.h:12`、`demo/device/src/config/device_config.c:9`。
- PC contract：`demo/pc/share/protocol-contract.json:1`。
- 设备 contract 模板：`demo/device/cmake/protocol-contract.json.in:1`。
- golden：`demo/integration_tests/contracts/protocol-contract.golden.json:1`。

## 3. 文件所有权

允许修改：

- `demo/pc/include/pc/protocol.h`
- `demo/device/include/protocol.h`
- `demo/pc/include/pc/params.h`
- `demo/pc/common/params.c`
- `demo/device/include/device_config.h`
- `demo/device/src/config/device_config.c`
- `demo/pc/config/pc_config.json`
- `demo/pc/config/pc_config.default.json`
- `demo/device/config/device_sim.json`
- `demo/device/config/device_linux.json`
- `demo/pc/share/protocol-contract.json`
- `demo/device/cmake/protocol-contract.json.in`
- `demo/integration_tests/contracts/protocol-contract.golden.json`
- 相邻配置/协议单元测试与测试 CMake。

禁止修改业务 core、backend、UI、runner 和 ESP32 目录。

## 4. 完整契约

必须逐字遵守 `Document/Update/beta v1.1/设计文档.md` 第 6、10 节。

### 4.1 常量

两端都定义：

```c
#define PROTO_VERSION 1
#define PROTO_MSG_MAX_LEN 1024
#define PROTO_FRAME_HEAD_LEN 2
#define PROTO_TCP_PORT 5935
#define PROTO_PC_AP_IP "192.168.137.1"
#define PROTO_PC_AP_PREFIX "Modu_"
#define PROTO_PC_AP_DEFAULT_SSID "Modu_PC"
#define PROTO_PC_AP_PASSWORD "modu_leventure"
#define PROTO_HOST_MAX_CONN 1
#define PROTO_SESSION_ID_LEN 8
#define APP_DATA_TEXT_MAX 512
```

commands 只保留 `CMD_DEVICE_HELLO`、`CMD_HOST_ACK`、`CMD_PING`、`CMD_PONG`、`CMD_APP_DATA`。

设备状态：

```c
DEV_STATE_BOOT=0,
DEV_STATE_WIFI_SCAN=1,
DEV_STATE_STA_JOIN=2,
DEV_STATE_CONNECT=3,
DEV_STATE_SESSION=4,
DEV_STATE_HEAL=5,
DEV_STATE_COUNT=6
```

### 4.2 PC 配置

按设计文档 6.2 裁剪 `demo_params_t`，新增：

```c
int params_validate(const demo_params_t *params, char *error, int error_capacity);
```

### 4.3 设备配置

按设计文档 6.3 裁剪 `device_config_t`，新增：

```c
int device_config_validate(const device_config_t *cfg, char *error, int error_capacity);
```

### 4.4 JSON contract

三份 contract 使用 schema 2，除 `generated_from` 外语义完全相等。结构以设计文档 10.4 为准，不含 discovery、persistence_keys、旧 commands、多连接容量。

## 5. 输入输出

输入：阶段一决策与设计文档。

输出：可被双方后续任务直接 include/load 的头文件和配置；三份语义一致的 JSON contract。

## 6. 错误处理

- validate 的 NULL 参数或 error buffer 非法返回 `DEMO_ERR_INVAL`。
- 配置不满足固定值返回 `DEMO_ERR_INVAL`，error 给出字段级原因。
- 密码错误提示只能写“热点密码必须使用产品固定值”，不得回显输入密码。
- 旧未知 JSON 字段可忽略，但已删除字段不得继续写入默认配置和 contract。

## 7. 单元测试

PC 与设备都覆盖：

1. 默认配置 validate 成功。
2. `Modu_PC` 成功。
3. 无 `Modu_` 前缀失败。
4. 非法后缀字符失败。
5. 密码非 `modu_leventure` 失败且消息不泄露密码。
6. IP、prefix、port 任一不符失败。
7. 空 SSID、超长 SSID失败。
8. contract JSON schema=2 且 commands/states 正确。

实际构建并运行对应 PC/设备配置与协议单元测试。

## 8. 验收标准

- 两端常量和值完全一致。
- 三份 contract 语义一致。
- 默认配置分别能支持 PC sim、PC Windows、设备 sim、设备 Linux。
- 不再定义 AP_PROVISION/DISCOVERY 和旧配网/发现命令。
- 单元测试正常与边界路径通过。

## 9. 完成报告要求

报告修改文件、关键契约、测试命令/结果、任何未解决问题。遇到设计文档冲突立即停止并报告 MainAgent，不得自行改跨端值。