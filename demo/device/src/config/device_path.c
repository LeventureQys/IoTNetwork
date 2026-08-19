#include "device_path.h"
#include "common.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

int device_path_is_absolute(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return 0;
#ifdef _WIN32
    if (path[0] == '/' || path[0] == '\\')
        return 1;
    if (path[1] == ':' && (path[2] == '/' || path[2] == '\\'))
        return 1;
    return 0;
#else
    return path[0] == '/';
#endif
}

int device_path_absolute(const char *path, char *out, size_t cap)
{
    if (path == NULL || out == NULL || cap == 0)
        return DEMO_ERR_INVAL;
    if (path[0] == '\0')
        return DEMO_ERR_INVAL;
#ifdef _WIN32
    /* 中间缓冲：GetFullPathNameA 不支持输入输出同一缓冲区。
     * 容量与 Linux PATH_MAX 路径解析一致（1024），避免深目录绝对路径被截断。 */
    {
        char full[1024];
        DWORD n = GetFullPathNameA(path, (DWORD)sizeof(full), full, NULL);
        if (n == 0 || n >= sizeof(full))
            return DEMO_ERR;
        if ((size_t)n >= cap)
            return DEMO_ERR;
        memcpy(out, full, (size_t)n + 1);
        return DEMO_OK;
    }
#else
    {
        char resolved[PATH_MAX];
        if (path[0] != '/') {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)) == NULL)
                return DEMO_ERR;
            if (snprintf(resolved, sizeof(resolved), "%s/%s", cwd, path) >= (int)sizeof(resolved))
                return DEMO_ERR;
        } else {
            if (snprintf(resolved, sizeof(resolved), "%s", path) >= (int)sizeof(resolved))
                return DEMO_ERR;
        }
        if (strlen(resolved) >= cap)
            return DEMO_ERR;
        memcpy(out, resolved, strlen(resolved) + 1);
        return DEMO_OK;
    }
#endif
}

int device_path_dirname(const char *path, char *out, size_t cap)
{
    if (path == NULL || out == NULL || cap == 0 || path[0] == '\0')
        return DEMO_ERR_INVAL;
    size_t len = strlen(path);
    size_t cut = len;
    while (cut > 0) {
        char c = path[cut - 1];
        if (c == '/' || c == '\\')
            break;
        cut--;
    }
    if (cut == 0) {
        out[0] = '\0';
        return DEMO_OK;
    }
    size_t dlen = cut - 1;
    if (dlen >= cap)
        return DEMO_ERR;
    memcpy(out, path, dlen);
    out[dlen] = '\0';
    return DEMO_OK;
}

int device_path_join(const char *dir, const char *name, char *out, size_t cap)
{
    if (dir == NULL || name == NULL || out == NULL || cap == 0)
        return DEMO_ERR_INVAL;
    if (name[0] == '\0') {
        if (strlen(dir) >= cap)
            return DEMO_ERR;
        memcpy(out, dir, strlen(dir) + 1);
        return DEMO_OK;
    }
    if (device_path_is_absolute(name)) {
        if (strlen(name) >= cap)
            return DEMO_ERR;
        memcpy(out, name, strlen(name) + 1);
        return DEMO_OK;
    }
    int need_sep = 1;
    size_t dlen = strlen(dir);
    if (dlen > 0) {
        char last = dir[dlen - 1];
        if (last == '/' || last == '\\')
            need_sep = 0;
    }
    if (dlen + (need_sep ? 1 : 0) + strlen(name) >= cap)
        return DEMO_ERR;
    memcpy(out, dir, dlen);
    size_t off = dlen;
    if (need_sep) {
#ifdef _WIN32
        out[off++] = '\\';
#else
        out[off++] = '/';
#endif
    }
    memcpy(out + off, name, strlen(name) + 1);
    return DEMO_OK;
}

int device_path_mkdirs(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return DEMO_ERR_INVAL;
    char tmp[1024];
    if (strlen(path) >= sizeof(tmp))
        return DEMO_ERR;
    memcpy(tmp, path, strlen(path) + 1);
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[len - 1] = '\0';
#ifdef _WIN32
    for (char *p = tmp + 3; *p; p++) { /* 跳过盘符/UNC 前缀 */
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            CreateDirectoryA(tmp, NULL); /* 已存在则忽略 */
            *p = save;
        }
    }
    if (CreateDirectoryA(tmp, NULL) == 0 &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return DEMO_ERR;
    return DEMO_OK;
#else
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            char save = *p;
            *p = '\0';
            mkdir(tmp, 0755);
            *p = save;
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return DEMO_ERR;
    return DEMO_OK;
#endif
}

int device_path_exists(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return 0;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

int device_path_remove(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return DEMO_ERR_INVAL;
#ifdef _WIN32
    if (DeleteFileA(path) == 0) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                   ? DEMO_OK
                   : DEMO_ERR;
    }
    return DEMO_OK;
#else
    if (unlink(path) != 0 && errno != ENOENT)
        return DEMO_ERR;
    return DEMO_OK;
#endif
}
