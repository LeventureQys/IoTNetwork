#include "pc_paths.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int is_abs(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 1;
    /* 盘符形式 C:\... C:/... */
    if (path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') ||
                           (path[0] >= 'a' && path[0] <= 'z')))
        return 1;
    return 0;
}

static void norm(const char *path, char *out, size_t cap)
{
    if (cap == 0)
        return;
    size_t n = strlen(path);
    for (size_t i = 0; i < n && i + 1 < cap; i++) {
        char ch = path[i];
        if (ch == '\\')
            ch = '/';
        out[i] = ch;
    }
    size_t len = n < cap - 1 ? n : cap - 1;
    out[len] = '\0';
    /* 去末尾分隔符（根 / 或 C:/ 保留） */
    if (len > 1 && out[len - 1] == '/')
        out[len - 1] = '\0';
}

void pc_path_normalize(const char *path, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
    if (path == NULL || path[0] == '\0') {
        out[0] = '\0';
        return;
    }
    norm(path, out, cap);
}

void pc_path_parent_dir(const char *path, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
    out[0] = '\0';
    if (path == NULL || path[0] == '\0')
        return;
    char normed[520];
    norm(path, normed, sizeof(normed));
    const char *slash = strrchr(normed, '/');
    if (slash == NULL) {
        out[0] = '\0';
        return;
    }
    if (slash == normed) {
        snprintf(out, cap, "%s", "/");
        return;
    }
    size_t len = (size_t)(slash - normed);
    if (len >= cap)
        len = cap - 1;
    memcpy(out, normed, len);
    out[len] = '\0';
}

int pc_path_absolute(const char *input, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return DEMO_ERR_INVAL;
    if (input == NULL || input[0] == '\0') {
        out[0] = '\0';
        return DEMO_ERR_INVAL;
    }
    char cwd[520];
#ifdef _WIN32
    if (GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd) == 0)
        return DEMO_ERR;
#else
    if (getcwd(cwd, sizeof(cwd)) == NULL)
        return DEMO_ERR;
#endif
    return pc_path_resolve(cwd, input, out, cap);
}

int pc_path_resolve(const char *base_dir, const char *input, char *out, size_t cap)
{
    if (out == NULL || cap == 0)
        return DEMO_ERR_INVAL;
    if (input == NULL || input[0] == '\0') {
        out[0] = '\0';
        return DEMO_ERR_INVAL;
    }
    if (is_abs(input)) {
        norm(input, out, cap);
        return DEMO_OK;
    }
    if (base_dir == NULL || base_dir[0] == '\0') {
        norm(input, out, cap);
        return DEMO_OK;
    }
    char base[520];
    norm(base_dir, base, sizeof(base));
    size_t base_len = strlen(base);
    size_t in_len = strlen(input);
    if (base_len + 1 + in_len + 1 > cap) {
        if (cap > 0)
            out[0] = '\0';
        return DEMO_ERR_NOMEM;
    }
    if (base[base_len - 1] == '/')
        snprintf(out, cap, "%s%s", base, input);
    else
        snprintf(out, cap, "%s/%s", base, input);
    /* 规范化内部 \ 与尾部 / */
    char normed[1040];
    norm(out, normed, sizeof(normed));
    snprintf(out, cap, "%s", normed);
    return DEMO_OK;
}
