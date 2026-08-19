#ifndef DEMO_LINUX_EXEC_H
#define DEMO_LINUX_EXEC_H

/*
 * 真实 Linux 后端共用的安全子进程调用层。
 *
 * 所有外部命令（nmcli/iw/ip/iptables/hostapd/dnsmasq/kill）一律以
 * argv 数组 + fork/execvp 方式调用，绝不经过 shell，避免拼接 shell 字符串
 * 造成的注入与转义问题。仅 Linux 编译。
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 以 argv 执行外部命令。argv 以 NULL 结尾，argv[0] 为可执行名
 * （execvp 按 PATH 搜索；含 '/' 时视为显式路径）。
 *
 * output 非空时捕获子进程 stdout，至多 output_capacity-1 字节并 NUL 终止；
 * 子进程退出前关闭全部继承的 fd（含输出管道写端之外的一切）。
 *
 * 返回：
 * - 子进程正常退出：退出码（0..255）；
 * - fork/exec/管道失败或子进程被信号终止：-1。
 */
int linux_exec_argv(const char *const argv[], char *output, size_t output_capacity);

#ifdef __cplusplus
}
#endif

#endif
