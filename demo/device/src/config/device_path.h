#ifndef DEMO_DEVICE_DEVICE_PATH_H
#define DEMO_DEVICE_DEVICE_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 显式路径工具：不 chdir、不向上搜索仓库。 */

int device_path_is_absolute(const char *path);
/* 相对路径按进程启动 CWD 解析为规范化绝对路径；绝对路径原样规范化。
 * 返回 DEMO_OK / DEMO_ERR_INVAL / DEMO_ERR（空间不足等）。 */
int device_path_absolute(const char *path, char *out, size_t cap);
int device_path_dirname(const char *path, char *out, size_t cap);
int device_path_join(const char *dir, const char *name, char *out, size_t cap);
int device_path_mkdirs(const char *path);
int device_path_exists(const char *path);
int device_path_remove(const char *path);

#ifdef __cplusplus
}
#endif

#endif
