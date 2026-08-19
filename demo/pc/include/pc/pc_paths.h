#ifndef PC_PATHS_H
#define PC_PATHS_H
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 显式运行路径（设计文档第 10 节）：
 *  - CLI 相对路径以进程启动 CWD 解析并立即规范化为绝对路径；
 *  - 配置文件内部相对路径以配置文件目录解析；
 *  - 不 chdir、不向上搜索仓库。
 * 全部实现为纯函数（不查询真实进程路径），便于测试。 */

/* 以 CWD 为基准解析为绝对路径（已绝对则原样规范）。成功返回 DEMO_OK，容量不足 DEMO_ERR_NOMEM。 */
int pc_path_absolute(const char *input, char *out, size_t cap);

/* 相对路径以 base_dir 为基准拼接为绝对路径；input 已绝对则原样规范。 */
int pc_path_resolve(const char *base_dir, const char *input, char *out, size_t cap);

/* 提取路径的父目录（不含文件名）；无目录部分时返回空串。 */
void pc_path_parent_dir(const char *path, char *out, size_t cap);

/* 规范化：统一为 / 分隔并去除末尾分隔符（根保留）。 */
void pc_path_normalize(const char *path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
