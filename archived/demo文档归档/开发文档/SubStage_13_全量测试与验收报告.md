# SubStage_13：全量测试、启动测试与验收报告

## SubStage: 全量测试与验收报告
- 所属 Stage：Stage 6
- 依赖前置：SubStage_12（可运行程序 + 剧本）
- 并行状态：需等待 Stage 5 完成
- 所属阶段：阶段三 - 开发（收尾）

## 1. 背景与目标

补齐/收敛全部单元测试（各 SubStage 自测 + 集成测试），执行启动测试（实际运行 provision_demo.exe 走核心链路，日志为证据），产出 `验收报告.md`（阶段四依据）。本任务书完成后阶段三结束，提交用户检查代码运行状态与测试结果。

## 2. 当前代码状态

- 已有：Stage 1~5 全部代码与各 SubStage 自测。
- 依据：`设计文档.md` 第 7 节（测试策略）；`验收文档.md`（验收项清单）；开发规范 3.3/4（测试与启动测试强制）。

## 3. 交付物

```
demo/tests/test_frame.cpp            （SubStage_01 已建，收敛）
demo/tests/test_params.cpp           （SubStage_01 已建，收敛）
demo/tests/test_protocol.cpp         （SubStage_01 已建，收敛）
demo/tests/test_net_abstraction.cpp  （SubStage_02 已建）
demo/tests/test_sim_world.cpp        （SubStage_03 已建）
demo/tests/test_sim_socket.cpp       （SubStage_03 已建）
demo/tests/test_sim_nvs.cpp          （SubStage_03 已建）
demo/tests/test_eventlog.cpp         （SubStage_05 已建）
demo/tests/test_limits.cpp           （SubStage_05 已建）
demo/tests/test_device_sm.cpp        （SubStage_05 已建）
demo/tests/test_provision.cpp        （SubStage_06 已建）
demo/tests/test_discovery.cpp        （SubStage_07 已建）
demo/tests/test_session.cpp          （SubStage_08 已建）
demo/tests/test_registry.cpp         （SubStage_09 已建）
demo/tests/test_tcp_server.cpp       （SubStage_09 已建）
demo/tests/test_announcer.cpp        （SubStage_10 已建）
demo/tests/test_provision_client.cpp （SubStage_10 已建）
demo/tests/test_host_app.cpp         （SubStage_11 已建）
demo/tests/test_end_to_end.cpp       （本任务书新增：进程内全链路集成）
demo/验收报告.md                     （本任务书产出）
```

## 4. 本任务书新增内容

### 4.1 test_end_to_end.cpp（进程内全链路，不依赖 main）

```text
场景 1 完整链路（单设备）：
  SimWorld 目标网络注册 → 创建 host（HostApp）与 1 台 device（独立线程）→
  host 线程启动 → ProvisionAllDevices（main 线程阻塞配网）→ 等待 device 进入 SESSION
  （轮询 device_app_get_state，超时 30s）→ 断言 host OnlineCount()==1 →
  触发 host RequestStop → 等待 device 收到 host_bye（组播）后转候选直连状态机不崩溃 →
  清理线程与实例。断言全链路各阶段事件日志存在。

场景 2 断线重连（单设备）：
  完成场景 1 至 SESSION → net_inject("wifi_disconnect") → 等待设备回到 SESSION
  （轮询，超时 60s，期间应经历 HEAL → STA_JOIN/DISCOVERY）→ wifi_ok 恢复 →
  断言再次 session established 且 host 侧 reconnect_count ≥ 1、session 续接
  （hello 携带 session_id）。

场景 3 多设备（3 台）：
  同上创建 3 台 → 全部进入 SESSION → host OnlineCount()==3 → 摘要日志。

场景 4 组播屏蔽兜底：
  配网完成后（设备已写候选列表）→ mcast_block → host 重启（RequestStop + 重建 HostApp
  不广播 bye——用 host_crash 语义）→ 设备经候选列表直连重新注册 → OnlineCount()==1。
```

- 每个场景独立 gtest TEST_F，超时断言失败（FAIL()）并清理所有线程/实例，避免悬挂。
- 测试日志级别 TRACE 输出到测试报告。

### 4.2 启动测试（人工/脚本执行，证据入验收报告）

```powershell
# 场景 A：默认配置单设备完整链路（45s 自动退出）
.\build\bin\provision_demo.exe --config config\demo_config.json --duration 45
# 期望：退出码 0；日志含 AP started → provision ok → state -> discovery →
#       session established → ping… → host_bye sent → 摘要

# 场景 B：演示剧本（故障注入 + 自愈）
.\build\bin\provision_demo.exe --config config\demo_config.json --scenario config\demo_scenario.json --duration 90
# 期望：日志含 scenario fired（wifi_disconnect / rssi_set / wifi_ok / host_stop）、
#       reconnect 事件、rssi degraded、host_bye sent

# 场景 C：多设备（3 台）
.\build\bin\provision_demo.exe --config config\demo_config.json --devices 3 --duration 60
# 期望：3 台全部 session established；摘要 3 条 online

# 场景 D：全部单测
.\build\bin\provision_demo_tests.exe
# 期望：全部通过，0 失败
```

- 启动测试实际执行，日志保存到 `run/launch_*.log`，关键行摘录进验收报告。

### 4.3 验收报告.md（零上下文格式）

按 `验收文档.md` 的验收项逐条填写：编译测试（全量构建无警告）、启动测试（场景 A~C 证据）、功能完整性（验收项 A1~An 通过/失败/阻塞 + 证据）、整体（回归：主项目 TactileSense 不受影响——Demo 独立工程，声明不影响主项目构建）、交付物检查、失败处理记录。

## 5. 验收标准

1. `provision_demo_tests.exe` 全部测试通过（含新增 test_end_to_end 4 场景）。
2. 启动测试场景 A~C 全部执行成功，证据日志齐全。
3. `验收报告.md` 完成，无"默认通过"项；硬件类验收项标注 N/A 原因（模拟环境）。
4. 阶段三结束门禁：代码运行状态 + 测试结果提交用户检查。

## 6. 禁止事项

- 不新增未经验收的功能（本任务书只做测试与报告）。
- 不修改产品代码以"让测试通过"（发现缺陷 → 修复产品代码并回归）。
- 不跳过启动测试（无硬件豁免理由——全模拟环境）。
- 验收报告中不写主观表述（按验收文档通过/失败标准填写）。

## 7. 依赖前置

- 等待 SubStage_12 完成后开工；MainAgent 在集成阶段先行合并 device_priv.h 补充字段（mcast_sock、candidate_list、busy_pending、err_* 计数、last_mcast_poll_ms）与 DEMO_SM_SUBMODULES 开关。
