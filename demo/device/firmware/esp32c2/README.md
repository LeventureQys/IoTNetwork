# firmware/esp32c2：ESP32-C2 设备 ESP-IDF 工程入口（骨架）

## 状态

**骨架工程，未在真实硬件验证；宿主（Windows/Linux）上无法执行 `idf.py build`，
需要 ESP-IDF 工具链 + 目标板（esp32c2/esp8684）。**

SS03 落地前本工程处于预期阻塞：`main/CMakeLists.txt` 引用的
`device/src/common`、`device/src/core`、`device/include` 尚不存在
（由 SS03 迁移 `demo/common`、`demo/include`、`demo/device/device_*.c` 产生）。
SS03 落地后可执行：

```bash
cd demo/device/firmware/esp32c2
idf.py set-target esp32c2
idf.py build
idf.py -p COMx flash monitor
```

## 目录结构

```text
firmware/esp32c2/
├── CMakeLists.txt        # ESP-IDF 工程入口（idf project）
├── sdkconfig.defaults    # esp32c2 / 2MB flash / mdns 默认配置
├── idf_component.yml     # 组件依赖：idf >=5.0、mdns
├── main/
│   ├── CMakeLists.txt    # main 组件：app_main.c + backends/esp32c2 + src/common/core
│   └── app_main.c        # 纯 C 入口：esp32c2_backend_get() + 平台无关 runtime
└── README.md
```

## 组合方式（设计文档 §8.6）

- 不使用宿主 dispatcher：`device_backend_create_for_host` 对 ESP32C2 恒返回
  `DEVICE_ERR_NOT_SUPPORTED`；
- `app_main.c` 直接组合 `esp32c2_backend_get()` 返回的 `net_backend_t` 与
  设备平台无关 runtime（`net_ctx_create` + SS03 的 core API）；
- 工程只引用 `device` 目录内部（`backends/esp32c2`、`include`、`src/common`、
  `src/core`），不引用仓库根目录或任何 C++/Qt。

## 已知阻塞与验收口径

| 项 | 说明 |
|----|------|
| `idf.py build` | 阻塞：当前机器无 ESP-IDF 工具链；且 SS03 的 `src/common/core` 尚未落地 |
| flash / SoftAP / DHCP / STA / NVS / 会话 | 信息性硬件现状检查，不属于本版本通过条件；骨架未实现这些能力，不得宣称可用 |
| 宿主侧检查 | 由 `demo/device/tests/esp32` 的宿主检查目标做纯 C 语法/接口完整性检查，不得报告为 ESP-IDF build 通过 |
