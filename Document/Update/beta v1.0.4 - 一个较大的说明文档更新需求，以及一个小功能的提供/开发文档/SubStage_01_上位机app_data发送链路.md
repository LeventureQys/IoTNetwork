# SubStage_01：上位机 app_data 联调消息发送链路

## SubStage: 上位机 app_data 发送链路（核心 + UI + 测试）
- 所属 Stage：Stage 1
- 依赖前置：无依赖（独立可执行，可立即开工）
- 并行状态：可与 SubStage_02 / SubStage_03 并行
- 所属阶段：阶段三 - 开发

## 1. 背景与目标

为上位机界面增加"直接发送消息"功能：配网完成、设备会话建立后，操作员可在界面选中某在线设备并发送一段调试文本，用于两端联调。消息复用协议既有 `app_data` 占位命令，走既有 TCP 业务连接。你负责上位机侧全部改动（核心发送链路、接收展示升级、UI 入口、测试）。

## 2. 当前代码状态

| 文件 | 现状 |
|------|------|
| `demo/pc/core/host_tcp_server.h`（48 行） | `HostTcpServer` 类：`Poll(uint64_t now_ms)` 在 host 线程被 `HostApp::Run` 周期调用；私有 `int SendFrame(void *sock, cJSON *obj)`；成员 `net_`/`reg_`/`params_`/`pending_`/`conn_rx_` |
| `demo/pc/core/host_tcp_server.cpp`（348 行） | `HandleOnline(DeviceEntry &e, uint64_t now_ms)` 中 `CMD_APP_DATA` 分支（约 317 行）：`LOG_I("HOST", "收到来自 %s 的 app_data 占位消息（已忽略）", e.id.c_str())` |
| `demo/pc/core/host_app.h/.cpp`（60/134 行） | `HostApp` 持有 `tcp_server_`；已有门面方法如 `SetBusyOverride`（转发风格参照它） |
| `demo/pc/ui/host_window.h/.cpp`（50/197 行） | `HostWindow`：`device_table_`（7 列 QTableWidget，Refresh 中以 `host_->registry().Snapshot()` 填充，行序 = Snapshot 顺序）；构造函数中 `buttons` 横向布局含配网按钮、停止按钮、stretch |
| `demo/pc/core/host_registry.h` | `DeviceEntry{ id, state("online"/"offline"), conn, ... }`；`HostRegistry::Snapshot()` 锁内完整拷贝；`Find(id)` 返回指针 |
| `demo/include/protocol.h` | `CMD_APP_DATA "app_data"`、`PROTO_MSG_MAX_LEN 1024` 已存在 |
| `demo/tests/test_host_app.cpp`（286 行） | 含 `E2EFixture`（启动 host 线程 + dev0 设备线程、自动配网至会话）、`LogCaptured(needle)`/`LogCount` 日志断言助手 |

线程模型：`HostApp::Run()` 在独立 host 线程；UI 在 Qt 主线程。**UI 线程禁止直接写 socket**。

## 3. 跨 SubStage 契约（与设备端 SubStage_02 共享，不得更改）

- 报文：`{"cmd":"app_data","seq":N,"data":{"type":"debug_text","text":"<UTF-8 文本>"}}`
  - `seq`：发送方自增，从 1 开始；接收方不校验。
  - `data.type` 固定 `"debug_text"`。
- 传输：既有业务 TCP 连接，2 字节大端长度前缀帧；JSON 负载 ≤ 1024 B。
- UI 文本上限常量：`#define APP_DATA_TEXT_MAX 512`（字节），由你添加到 `demo/include/protocol.h`（放在 `PROTO_SESSION_ID_LEN` 定义附近）。设备端并行开发时会直接使用此定义；若你发现该宏已被添加（集成冲突），保留单一定义即可。
- 设备侧接收日志文案（用于你的 E2E 断言，由 SubStage_02 实现）：包含"收到上位机调试消息：<text>"。
- 上位机侧接收日志文案（你实现，SubStage_02 的 E2E 断言依赖它）：`LOG_I("HOST", "收到来自 %s 的调试消息：%s", 设备id, text)`。

## 4. 实现要求

### 4.1 `demo/include/protocol.h`

新增一行：`#define APP_DATA_TEXT_MAX 512`。不得改动其他任何常量。

### 4.2 `demo/pc/core/host_tcp_server.h/.cpp`

1. 新增成员（private）：
   ```cpp
   std::mutex tx_mu_;
   std::vector<std::pair<std::string, std::string>> pending_tx_; /* (device_id, text) */
   uint32_t app_data_seq_ = 0;
   ```
   （头文件补 `#include <mutex>`/`<utility>` 如需。）
2. 新增公开方法：
   ```cpp
   int QueueAppData(const std::string &device_id, const std::string &text);
   ```
   行为（UI 线程调用）：`device_id` 为空、`text` 为空或 `text.size() > APP_DATA_TEXT_MAX` → 返回 `DEMO_ERR`；否则 `tx_mu_` 加锁压入 `pending_tx_`，返回 `DEMO_OK`。此方法不做设备存在性检查（冲刷时处理）。
3. `Poll(uint64_t now_ms)` 末尾新增冲刷逻辑：
   - `tx_mu_` 加锁取走 `pending_tx_`（swap 到局部变量）后解锁；
   - 逐条：`reg_.Find(device_id)`；条目存在且 `state=="online"` 且 `conn!=nullptr` → 构造 cJSON：`cmd=CMD_APP_DATA`、`seq=(int)++app_data_seq_`、`data` 对象含 `type="debug_text"`、`text=text` → 复用 `SendFrame(entry->conn, obj)`；成功 `LOG_I("HOST", "已向 %s 发送调试消息（%zu 字节）", ...)`；`SendFrame` 失败 → `LOG_W` 丢弃。条目不存在/离线 → `LOG_W("HOST", "调试消息丢弃：%s 不在线", ...)`。
4. `HandleOnline` 中 `CMD_APP_DATA` 分支改为：解析 `data`（对象）→ `text`（字符串）；两者均有效 → `LOG_I("HOST", "收到来自 %s 的调试消息：%s", e.id.c_str(), text)`；否则保留原占位日志行。

错误处理契约：`QueueAppData`/冲刷均不抛异常、不崩溃；失败路径只记日志。

### 4.3 `demo/pc/core/host_app.h/.cpp`

新增公开门面（参照 `SetBusyOverride` 的转发风格）：

```cpp
int SendAppDataToDevice(const std::string &id, const std::string &text); /* 转发 tcp_server_.QueueAppData */
```

### 4.4 `demo/pc/ui/host_window.h/.cpp`

1. 成员：`QLineEdit *msg_edit_;`、`QPushButton *msg_send_button_;`、槽 `void SendAppData();`。
2. 构造函数中创建控件：placeholder "输入发给所选设备的联调消息（≤512 字节）"；按钮文本"发送消息"；加入既有 `buttons` 布局（配网按钮、停止按钮之后，`addStretch()` 之前）；`connect(msg_send_button_, &QPushButton::clicked, this, &HostWindow::SendAppData)`。
3. `SendAppData()` 槽：
   - `row = device_table_->currentRow()`；`row < 0` → `LOG_W("MAIN", "请先在设备表中选择要发送消息的设备")`，返回。
   - `entries = host_->registry().Snapshot()`；`row >= entries.size()` → 同上警告，返回。
   - `entries[row].state != "online"` → `LOG_W("MAIN", "设备 %s 当前离线，无法发送消息", ...)`，返回。
   - `text = msg_edit_->text().toUtf8()` 转 `std::string`；`rc = host_->SendAppDataToDevice(entries[row].id, text)`；`rc == DEMO_OK` → `LOG_I("MAIN", "上位机界面：联调消息已提交发送（目标=%s）", ...)`；否则 → `LOG_W("MAIN", "发送失败：消息为空或超过 %d 字节", APP_DATA_TEXT_MAX)`。
   - 发送成功不清空输入框。

### 4.5 测试（`demo/tests/test_host_app.cpp`，复用 `E2EFixture`）

在文件末尾追加三个用例（**含 SubStage_02 的 E2E 用例，你按契约编写；若 SubStage_02 的 `device_app_send_app_data` 尚不存在导致编译不过，将该用例用注释占位并在完成报告中注明，由 MainAgent 集成时启用**）：

1. `TEST(HostAppData, SendToDevice)`：`f.Start()` 后等待会话（轮询 `f.host->OnlineCount() == 1`，上限 15s）→ `f.host->SendAppDataToDevice("02:00:00:00:00:01", "hello-device")` → 轮询等待 → `EXPECT_TRUE(LogCaptured("收到上位机调试消息：hello-device"))`。
2. `TEST(HostAppData, RejectInvalid)`：不启动完整 E2E 亦可——直接构造最小 fixture 或复用 `f.Start()` 后：`SendAppDataToDevice(id, "")` 与 513 字节字符串均返回 `DEMO_ERR`；`SendAppDataToDevice("ff:ff:ff:ff:ff:ff", "x")` 返回 `DEMO_OK`（入队）但冲刷时产生 `LogCaptured("调试消息丢弃")`。
3. `TEST(DeviceAppData, SendToHost)`：会话建立后 `device_app_send_app_data(f.dev, "hello-host")`（该函数由 SubStage_02 提供，声明于 `device_app.h`）→ 轮询等待 → `EXPECT_TRUE(LogCaptured("hello-host"))`。

## 5. 输入输出

- 输入：用户在 `msg_edit_` 输入的 UTF-8 文本；`device_table_` 选中的设备行。
- 输出：TCP 业务连接上的 `app_data` 帧；日志窗口的发送/接收记录。

## 6. 验收标准

1. `pc/` 与 `tests/` 项目编译通过，无新增警告。
2. 三个新测试用例实际运行并通过（`DeviceAppData.SendToHost` 若因 SubStage_02 未完成而注释占位，在完成报告中明确标注）。
3. 既有全部测试通过（回归零失败）。
4. UI 上"发送消息"入口位于按钮行；未选中/离线/超长均有日志提示且不崩溃。

## 7. 禁止事项

- 不改动任何协议常量（端口、命令名、帧格式）与既有测试用例。
- 不修改 `demo/device/` 下任何文件（设备端属 SubStage_02 边界）。
- 不修改 `demo/net_*` 后端与 `common/` 公共代码（protocol.h 仅允许新增一行宏）。
- 不做物理目录重组、不新增 CMake target（测试文件已被现有构建收录）。

## 8. 构建与自测命令（Windows 宿主）

```powershell
# 上位机
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="<Qt6路径>" -S demo/pc -B demo/pc/build
cmake --build demo/pc/build --config Debug -j8
# 测试（tests 为独立 CMake 工程，参照 demo/tests/CMakeLists.txt 既有配置）
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="<Qt6路径>" -S demo/tests -B demo/tests/build-msvc
cmake --build demo/tests/build-msvc --config Debug -j8
ctest --test-dir demo/tests/build-msvc -C Debug --output-on-failure
```

Linux 环境（rk3588 验证机）以该项目既有 cmake 配置等价执行。
