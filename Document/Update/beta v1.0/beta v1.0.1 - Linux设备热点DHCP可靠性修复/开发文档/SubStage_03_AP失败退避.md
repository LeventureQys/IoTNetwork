# SubStage 03：AP 启动失败退避

- 所属 Stage：Stage 1 - 纯逻辑与共用契约
- 依赖前置：无
- 并行状态：可与 SubStage 01、02 并行
- 所属阶段：阶段三 - 开发

## 1. 目标

修复永久热点错误导致约每 110 ms 重启 hostapd/dnsmasq和日志刷屏的问题，复用已有 `provision_ap_backoff_ms`。

## 2. 当前状态

`demo/device/device_app.c:621` 在 `prov_server_start()` 失败后只 sleep 100 ms 并留在 AP_PROVISION；主循环 `demo/device/device_app.c:819` 约 10 ms 后再次调用。

## 3. 文件范围

允许：

- `demo/device/device_app.c`
- 新增 `demo/tests/test_device_ap_backoff.cpp`，或在现有状态机测试中增加独立用例
- `demo/tests/CMakeLists.txt`

禁止：

- 修改 `device_provision.c` 协议处理
- 修改 PC 代码
- 新增退避配置字段

## 4. 行为契约

首次 AP start 失败后：

```c
evlog_record(app, "配网热点启动失败，等待退避后重试");
app->boot_backoff_until =
    now + (uint64_t)app->params->provision_ap_backoff_ms;
device_set_state(app, DEV_STATE_BOOT);
return;
```

- 不调用固定 `device_sleep_ms(100)`。
- 日志包含退避毫秒数或下一次重试时间。
- BOOT 退避期间不得调用 `wifi_ap_start`。
- 到期后按既有 BOOT 路径只进入一次 AP_PROVISION。
- `prov_server_start()` 已负责关闭失败时的 TCP listener，保持该行为。

## 5. 测试

- fake backend 永久返回 AP start 失败。
- 第一次失败后状态为 BOOT，deadline 正确。
- 退避前 start 调用次数不增长。
- 到期后仅新增一次尝试。
- 事件不在每个 tick 重复记录。
- TCP listener 关闭行为的既有测试继续通过。

## 6. 验收标准

- 无高频重试。
- 不改变 PIN 锁定、空闲超时或过早 close_ap 的既有退避路径。
- 所有状态机测试通过。
