#ifndef DEMO_LINUX_WIFI_H
#define DEMO_LINUX_WIFI_H

/*
 * 真实 Linux WiFi STA（客户端）模块，实例化版本。
 *
 * 使用 nmcli / ip / iw 管理真实 WiFi 连接、扫描、获取 IP/RSSI/SSID/网关。
 * 所有外部命令以 argv + fork/exec 调用（linux_exec.h），绝不经过 shell，
 * 因此 SSID/密码中的引号、$、反斜杠等字符不会被命令解释器二次解释；
 * 密码只作为 argv 元素传递，不写入日志。
 * 仅 Linux 下编译；Windows 平台不编译本模块（见 CMakeLists）。
 */

#include "net_abstraction.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * STA 命令执行 ops（供测试注入；NULL 使用 linux_exec_argv 生产实现）。
 * exec_argv：与 linux_exec_argv 同语义，output 捕获子进程 stdout。
 */
typedef struct linux_wifi_ops {
    int (*exec_argv)(const char *const argv[], char *output,
                     size_t output_capacity);
} linux_wifi_ops_t;

typedef struct linux_wifi linux_wifi_t;

/*
 * 创建 STA 实例：
 * - sta_interface 直接指定 STA 网卡名；为 NULL/空时回退 "wlP2p33s0"。
 *   beta v1.1 解耦：不再依赖 linux_hotspot_t（纯 STA 后端普通用户即可创建）；
 * - ops_override 非 NULL 时用于替换命令执行（仅供测试）。
 * 失败时 *out 置 NULL。
 */
int linux_wifi_create(linux_wifi_t **out, const char *sta_interface,
                      const linux_wifi_ops_t *ops_override);

void linux_wifi_destroy(linux_wifi_t *wifi);

int linux_wifi_connect(linux_wifi_t *wifi, const char *ssid,
                       const char *password, wifi_reason_t *reason);

int linux_wifi_disconnect(linux_wifi_t *wifi);

int linux_wifi_get_ip(linux_wifi_t *wifi, uint32_t *ip);

int linux_wifi_get_rssi(linux_wifi_t *wifi, int *rssi);

int linux_wifi_is_connected(linux_wifi_t *wifi);

int linux_wifi_is_connected_to(linux_wifi_t *wifi, const char *ssid);

int linux_wifi_get_current_ssid(linux_wifi_t *wifi, char *ssid, int capacity);

int linux_wifi_scan(linux_wifi_t *wifi, net_ap_info_t *aps, int *count);

int linux_wifi_get_gateway(linux_wifi_t *wifi, uint32_t *ip);

const char *linux_wifi_sta_interface(const linux_wifi_t *wifi);

#ifdef __cplusplus
}
#endif

#endif
