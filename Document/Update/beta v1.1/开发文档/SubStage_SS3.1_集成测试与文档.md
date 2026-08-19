# SubStage SS3.1：黑盒集成测试、启动测试与文档

## SubStage: 黑盒集成测试、启动测试与文档
- 所属 Stage：Stage 3 - 集成与测试
- 依赖前置：SS2.1、SS2.2、SS2.3、SS2.4 全部完成并通过端侧单元测试
- 并行状态：需等待全部 Stage 2 产物
- 所属阶段：阶段三 - 开发

## 1. 目标

重写旧配网场景为 PC 热点→设备扫描→固定IP TCP，并同步产品 README。集成测试仍只消费二进制、CLI、事件和 contract。

## 2. 当前代码

- 旧场景表：`demo/integration_tests/README.md:45`。
- b/e/f 依赖 auth/wifi_result/close_ap：`demo/integration_tests/README.md:50`。
- runner 输出证据结构：`demo/integration_tests/README.md:105`。

## 3. 文件所有权

允许修改 `demo/integration_tests/`（contract golden 已由SS1.1冻结，除非MainAgent授权）、根/pc/device README、必要的安装说明。禁止修改端侧生产源码；发现缺陷退回对应 SubStage。

## 4. 场景

- A：三份 schema2 contract语义一致。
- B：PC hotspot_ready→设备 wifi_target_found/wifi_connected→双方 session_online→ping/pong。
- C：PC正常退出，catalog删除，设备session_offline并继续扫描。
- D：TCP故障注入后固定地址重连。
- E：第二设备收到 single_device_only，第一设备在线。
- F：错误密码/目标SSID不存在，无session_online，设备不启动AP。
- G：双向app_data 512成功、513拒绝、UTF-8一致。
- H：拆包/粘包/空帧/超长/版本错误。

删除旧 e_close_ap_fail/f_wifi_result_fail，场景文件命名与README一致。

## 5. runner 契约

- 先启动PC sim并等待 hotspot_ready，再启动设备。
- 两端共享 sim-catalog-dir。
- 断言事件顺序，不只断言存在。
- summary.json 每断言含 name/status/evidence。
- 进程异常、超时、catalog残留、端口残留均失败。
- 不 include/编译端侧源码。

## 6. 启动测试

1. PC sim offscreen 3秒：热点发布/清理。
2. Device sim offscreen 3秒：无PC时持续扫描且正常退出。
3. 双进程B完整路径。
4. Windows真实热点：按验收文档执行并收集事件、日志、系统适配器/IP证据。
5. Linux真实设备：运行 `--backend linux`，收集nmcli连接、IP、session事件。

真实环境缺失时写明阻塞条件并通知MainAgent追加阶段三问题清单置顶，不得标通过。

## 7. 文档同步

更新根README、demo README、pc/device README：新拓扑、构建、CLI、配置、真实环境依赖、ESP32不在范围、测试命令和证据路径。删除旧SoftAP配网和服务发现说明。

## 8. 验收标准

- A-H sim场景通过。
- README与实际CLI/配置一致。
- 旧场景/文案不再作为生产说明。
- 真实环境结果有证据或明确阻塞。

## 9. 禁止事项

不修改端侧生产代码；不放宽断言迁就缺陷；不把sim描述为Windows/Linux真实测试；不宣称ESP32支持。

## 10. 完成报告

列出场景结果、证据目录、启动测试记录、真实环境状态和文档文件。