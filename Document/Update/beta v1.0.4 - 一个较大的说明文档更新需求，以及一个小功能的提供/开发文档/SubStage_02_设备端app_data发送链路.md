# SubStage_02：设备端 app_data 联调消息发送链路

## SubStage: 设备端 app_data 发送链路（核心 + UI + 测试）
- 所属 Stage：Stage 1
- 依赖前置：无依赖（独立可执行，可立即开工）
- 并行状态：可与 SubStage_01 / SubStage_03 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

为设备端界面增加"直接发送消息"功能：设备与上位机会话建立后，操作员可在设备模拟器界面输入调试文本发给上位机，用于两端联调；同时把设备侧对 `app_data` 消息的接收处理从"忽略"升级为"日志展示文本"。消息复用协议既有 `app_data` 占位命令，走既有会话 TCP 连接。你负责设备侧全部改动（纯 C 业务层 + Qt UI + 单元测试）。

## 2. 当前代码状态

| 文件 | 现状 |
|------|------|
| `demo/device/device_priv.h`（115 行） | `struct device_app`：阶段四会话区已有 `sess_sock`、`session_id`、`ping_seq` 等字段；文件已 include `protocol.h` |
| `demo/device/device_app.h/.c` | 公开 API：`device_app_create/run/get_state/...`；内部 `device_send_frame(app, sock, cJSON*)`、`device_set_state`。`device_app_get_state` 返回 `device_state_t` |
| `demo/device/device_session.h/.c`（203 行） | `session_poll(app)` 在 device 循环中周期调用（收包 + 心跳 + 判死）；`session_on_msg` 中 `CMD_APP_DATA` 分支（约 177 行）：`LOG_I(..., "会话：收到 app_data 占位消息（已忽略）")`；发送参照 `session_send_ping`（cJSON 构造 → `device_send_frame(app, app->sess_sock, obj)`） |
| `demo/device/device_limits.h` | `int limits_allow_send(device_app_t *app, int is_heartbeat);` 设备侧限速（非心跳传 0） |
| `demo/device/ui/device_window.h/.cpp`（48/152 行） | `DeviceWindow` 持有 `device_`；构造函数中 `controls` 横向布局含 `action_box_`、`argument_edit_`、注入按钮、停止按钮；`Refresh()` 每 150ms 轮询 `device_app_get_snapshot` |
| `demo/tests/test_device_logic.cpp`（154 行） | 含 `DevFixture`（`SetUp(nvs_tag, device_id, fresh)` 创建 sim 后端 + `device_app_create`；`TearDown` 清理），纯设备侧单测，不需要上位机 |
| `demo/include/protocol.h` | `CMD_APP_DATA "app_data"`、`DEV_STATE_SESSION` 已存在；`#define APP_DATA_TEXT_MAX 512` 由并行的 SubStage_01 添加 |

线程模型：`device_app_run` 在独立 device 线程（`device/app/main.cpp`）；UI 在 Qt 主线程。**UI 线程禁止直接写 socket**；跨线程传递用单槽请求缓冲，由 device 循环消费。

## 3. 跨 SubStage 契约（与上位机 SubStage_01 共享，不得更改）

- 报文：`{"cmd":"app_data","seq":N,"data":{"type":"debug_text","text":"<UTF-8 文本>"}}`
  - `seq`：发送方自增，从 1 开始；接收方不校验。
  - `data.type` 固定 `"debug_text"`。
- 传输：既有会话 TCP 连接（`app->sess_sock`），2 字节大端长度前缀帧（`device_send_frame` 已处理），JSON 负载 ≤ 1024 B。
- UI 文本上限常量：`APP_DATA_TEXT_MAX 512`，定义于 `demo/include/protocol.h`（由 SubStage_01 添加）。**若并行开发时该宏尚未存在**：以本任务书给出的定义为准在你的分支内临时补上（`#ifndef APP_DATA_TEXT_MAX #define APP_DATA_TEXT_MAX 512 #endif` 形式），并在完成报告中注明，MainAgent 集成时去重。
- 设备侧接收日志文案（你实现，SubStage_01 的 E2E 断言依赖它）：`LOG_I(app->device_id, "会话：收到上位机调试消息：%s", text)`。
- 上位机侧接收日志文案（SubStage_01 实现，你不得修改）：包含 `"收到来自 <id> 的调试消息：<text>"`。

## 4. 实现要求

### 4.1 `demo/device/device_priv.h`

在阶段四会话区（`sess_sock` 相关字段附近）新增：

```c
/* UI 联调消息（UI 线程提交，device 循环单槽消费；新请求覆盖未消费旧请求） */
char     ui_tx_text[APP_DATA_TEXT_MAX + 1];
int      ui_tx_pending;
uint32_t app_data_seq;
```

确认 `device_app_create` 现有初始化方式（若为 calloc 分配则天然为 0，无需额外代码，但在完成报告中说明；若为逐字段初始化则就近补齐三字段）。

### 4.2 `demo/device/device_app.h/.c`

新增公开 API：

```c
/* UI 线程调用：提交一条联调文本，由 device 循环在会话态发出。
 * 返回 DEMO_OK=已受理；DEMO_ERR=参数非法、超长或非会话状态 */
int device_app_send_app_data(device_app_t *app, const char *text);
```

行为：
- `app == NULL`、`text == NULL`、`text[0] == '\0'`、`strlen(text) > APP_DATA_TEXT_MAX` → 返回 `DEMO_ERR`。
- `device_app_get_state(app) != DEV_STATE_SESSION` → 返回 `DEMO_ERR`。
- 若 `app->ui_tx_pending == 1`（上一条未消费）：覆盖内容并 `LOG_W(app->device_id, "会话：上一条联调消息未发出，已被新消息覆盖")`。
- `snprintf(app->ui_tx_text, sizeof(app->ui_tx_text), "%s", text);` → `app->ui_tx_pending = 1;` → 返回 `DEMO_OK`。

### 4.3 `demo/device/device_session.c`

1. 新增静态函数 `session_flush_ui_tx(device_app_t *app)`，并在 `session_poll` 末尾（判死检查**之前**）调用：
   - `app->ui_tx_pending == 0` 或 `app->sess_sock == NULL` → 直接返回（sess_sock 为空时同时清 `ui_tx_pending`，防悬挂）。
   - `limits_allow_send(app, 0)` 返回 0（限速中）→ 本次跳过不清标志（下轮重试）。
   - 构造 cJSON：`cmd=CMD_APP_DATA`、`seq=(int)++app->app_data_seq`、`data` 对象含 `type="debug_text"`、`text=app->ui_tx_text` → `device_send_frame(app, app->sess_sock, obj)` → `cJSON_Delete`。
   - 发送成功 → 清 `ui_tx_pending`，`LOG_I(app->device_id, "会话：已向上位机发送调试消息（%d 字节）", (int)strlen(app->ui_tx_text))`；发送失败 → 同样清 `ui_tx_pending` 并 `LOG_W`（连接已断，不再重发）。
2. `session_on_msg` 中 `CMD_APP_DATA` 分支改为：解析 `data`（对象）与 `data.text`（字符串）；均有效 → `LOG_I(app->device_id, "会话：收到上位机调试消息：%s", text)`；否则保留原占位日志行。

### 4.4 `demo/device/ui/device_window.h/.cpp`

1. 成员：`QLineEdit *msg_edit_;`、`QPushButton *msg_send_button_;`、槽 `void SendAppData();`。
2. 构造函数中创建控件：placeholder "输入发给上位机的联调消息（≤512 字节）"；按钮文本"发送消息"；加入既有 `controls` 布局（保持单行，位置在注入按钮与停止按钮之间或停止按钮之前均可）；`connect(msg_send_button_, &QPushButton::clicked, this, &DeviceWindow::SendAppData)`。
3. `SendAppData()` 槽：
   - `text = msg_edit_->text().trimmed().toUtf8()`；
   - `rc = device_app_send_app_data(device_, text.constData())`；
   - `DEMO_OK` → `LOG_I("MAIN", "下位机界面：联调消息已提交发送")`；`DEMO_ERR` → `LOG_W("MAIN", "发送失败：未在会话状态或消息为空/超长")`。
   - 发送成功不清空输入框。
4. 头文件 `device_window.h` 前置声明已足够，不需要新增 include（`device_app.h` 已在 cpp 中包含）。

### 4.5 测试（`demo/tests/test_device_logic.cpp`，复用 `DevFixture`）

文件末尾追加两个用例：

1. `TEST(DeviceAppData, RejectWhenNotSession)`：`f.SetUp()` 后设备处于 BOOT/AP 态（未建立会话）→ `device_app_send_app_data(f.app, "hello")` 返回 `DEMO_ERR`。
2. `TEST(DeviceAppData, RejectEmptyAndTooLong)`：`f.SetUp()` 后 → 空串 `""` 返回 `DEMO_ERR`；构造 513 字节字符串（如 `std::string(513, 'a')`）返回 `DEMO_ERR`；`nullptr` 返回 `DEMO_ERR`。

**注意**：不要修改 `test_host_app.cpp`（属 SubStage_01 边界）；设备→上位机方向的 E2E 用例由 SubStage_01 按契约编写。

## 5. 输入输出

- 输入：操作员在 `msg_edit_` 输入的 UTF-8 文本。
- 输出：会话 TCP 连接上的 `app_data` 帧；日志窗口的发送/接收记录。

## 6. 验收标准

1. `device/` 与 `tests/` 项目编译通过，无新增警告；device 模块保持零平台依赖（不引入任何平台头）。
2. 两个新单元测试实际运行并通过。
3. 既有全部测试通过（回归零失败）。
4. UI 上"发送消息"入口位于控制行；非会话态点击仅产生警告日志，不崩溃。

## 7. 禁止事项

- 不改动任何协议常量与既有测试用例；不修改 `demo/pc/` 下任何文件。
- 不修改 `demo/net_*` 后端；device 模块新增代码不得引入 Qt/平台依赖（UI 改动仅限 `device/ui/`）。
- 不改动会话心跳、判死、重连等既有逻辑的行为。

## 8. 构建与自测命令（Windows 宿主）

```powershell
# 设备端
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="<Qt6路径>" -S demo/device -B demo/device/build
cmake --build demo/device/build --config Debug -j8
# 测试
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="<Qt6路径>" -S demo/tests -B demo/tests/build-msvc
cmake --build demo/tests/build-msvc --config Debug -j8
ctest --test-dir demo/tests/build-msvc -C Debug --output-on-failure
```

Linux 环境（rk3588 验证机）以该项目既有 cmake 配置等价执行。
