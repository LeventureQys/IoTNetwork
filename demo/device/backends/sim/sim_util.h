/* ============================================================================
 * sim_util.h - 内部共享平台小工具（纯 C11，头内 static inline）。
 *
 * 不构成独立模块：仅提供 backends/sim/** 各 .c 之间共享的
 * 时钟 / 休眠 / 进程 PID / 原子写文件 / 路径 / IPv4 小工具。
 * 禁止任何全局可变状态。
 * ========================================================================== */
#ifndef DEMO_DEVICE_SIM_UTIL_H
#define DEMO_DEVICE_SIM_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

/* 单调时钟（毫秒，进程内比较用；语义等价于旧实现的 steady_clock） */
static inline uint64_t sim_util_monotonic_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

/* 墙钟（毫秒，跨进程比较用；AP catalog published_at_ms 采用此时钟） */
static inline uint64_t sim_util_wallclock_ms(void)
{
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER ul;
    GetSystemTimeAsFileTime(&ft);
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    /* 1601-01-01 → 1970-01-01 的 100ns 间隔数 */
    return (uint64_t)((ul.QuadPart / 10000) - 11644473600000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static inline void sim_util_sleep_ms(unsigned int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep((useconds_t)ms * 1000u);
#endif
}

static inline unsigned long sim_util_current_pid(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

/* 线程唯一 ID：同进程并发发布者使用不同 tmp 名，避免互相碰撞 */
static inline unsigned long sim_util_current_tid(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentThreadId();
#else
    return (unsigned long)(uintptr_t)pthread_self();
#endif
}

/* 判断 pid 对应进程是否存活；无法判断时返回 0（视为不存在）。 */
static inline int sim_util_pid_alive(unsigned long pid)
{
    if (pid == 0)
        return 0;
#ifdef _WIN32
    {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (h) {
            CloseHandle(h);
            return 1;
        }
        return (GetLastError() == ERROR_ACCESS_DENIED) ? 1 : 0;
    }
#else
    if (kill((pid_t)pid, 0) == 0)
        return 1;
    return (errno == EPERM) ? 1 : 0;
#endif
}

static inline void sim_util_copy_bounded(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    snprintf(dst, cap, "%s", src);
}

/* 判断是否为绝对路径（Windows：盘符/UNC/根；POSIX：/ 开头） */
static inline int sim_util_is_absolute(const char *p)
{
    if (!p || !p[0])
        return 0;
#ifdef _WIN32
    if (p[0] == '/' || p[0] == '\\')
        return 1;
    if (p[1] == ':' && (p[2] == '/' || p[2] == '\\'))
        return 1;
    return 0;
#else
    return p[0] == '/';
#endif
}

/* 递归创建目录（已存在视为成功）；任一环节失败返回 -1 */
static inline int sim_util_mkdirs(const char *path)
{
    if (!path || !path[0])
        return -1;
#ifdef _WIN32
    {
        char tmp[512];
        size_t n = strlen(path);
        if (n >= sizeof(tmp))
            return -1;
        memcpy(tmp, path, n + 1);
        size_t i = 0;
        if (n >= 2 && tmp[1] == ':')
            i = 2;
        else if (n >= 2 && tmp[0] == '\\' && tmp[1] == '\\') {
            i = 2;
            while (i < n && tmp[i] != '\\' && tmp[i] != '/')
                i++;
        } else if (n >= 1 && (tmp[0] == '/' || tmp[0] == '\\'))
            i = 1;
        for (; i < n; i++) {
            if (tmp[i] == '\\' || tmp[i] == '/') {
                char saved = tmp[i];
                tmp[i] = 0;
                if (tmp[0])
                    CreateDirectoryA(tmp, NULL);
                tmp[i] = saved;
            }
        }
        DWORD attr = GetFileAttributesA(path);
        return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? 0 : -1;
    }
#else
    {
        char tmp[512];
        size_t n = strlen(path);
        if (n >= sizeof(tmp))
            return -1;
        memcpy(tmp, path, n + 1);
        size_t i = (tmp[0] == '/') ? 1 : 0;
        for (; i <= n; i++) {
            if (tmp[i] == '/' || i == n) {
                char saved = tmp[i];
                tmp[i] = 0;
                if (tmp[0])
                    mkdir(tmp, 0755);
                tmp[i] = saved;
            }
        }
        struct stat st;
        return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
    }
#endif
}

/* 同目录 tmp 文件 + flush + 原子替换写文件；失败清理 tmp 并返回 -1。
 * tmp 名：<path>.tmp-<pid>-<tid>（契约要求的 <name>.tmp-<pid> 形态并附加
 * 线程 ID，保证同进程多线程并发发布者不互相碰撞；读者一律跳过含 ".tmp-" 的文件）。
 * Windows 原子替换带有限重试（并发替换者可能瞬时共享冲突，数据均已 flush 完整）。 */
static inline int sim_util_atomic_write_file(const char *path,
                                             const char *content, size_t content_len)
{
    if (!path || !content)
        return -1;
    size_t n = strlen(path);
    if (n == 0 || n > 1024)
        return -1;
    char *tmp = (char *)malloc(n + 48);
    if (!tmp)
        return -1;
    snprintf(tmp, n + 48, "%s.tmp-%lu-%lu", path, sim_util_current_pid(),
             sim_util_current_tid());
    size_t slash = n;
    while (slash > 0 && path[slash - 1] != '/' && path[slash - 1] != '\\')
        slash--;
    if (slash > 0) {
        char dir[512];
        size_t dlen = slash;
        if (dlen >= sizeof(dir))
            dlen = sizeof(dir) - 1;
        memcpy(dir, path, dlen);
        dir[dlen] = 0;
        sim_util_mkdirs(dir);
    }
    int ok = 0;
    FILE *f = fopen(tmp, "wb");
    if (f) {
        if (fwrite(content, 1, content_len, f) == content_len && fflush(f) == 0 &&
            fclose(f) == 0)
            ok = 1;
        else
            fclose(f);
    }
    int rc = -1;
    if (ok) {
#ifdef _WIN32
        int attempt;
        for (attempt = 0; attempt < 100; attempt++) {
            if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
                rc = 0;
                break;
            }
            Sleep(1);
        }
#else
        if (rename(tmp, path) == 0)
            rc = 0;
#endif
    }
    if (rc != 0)
        remove(tmp);
    free(tmp);
    return rc;
}

/* 读取整个文件到 buf（cap 需含结尾 NUL 空间，即文件大小必须 < cap）。
 * Windows 以共享删除标志（FILE_SHARE_DELETE）打开，保证与原子替换并发
 * （发布者/读者交错）时读取不被阻塞、替换不被共享冲突打断。
 * 返回 0=成功（*out_len=字节数，buf 以 NUL 结尾）；-1=缺失/过大/读失败。 */
static inline int sim_util_read_file(const char *path, char *buf, size_t cap,
                                     size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!path || !buf || cap == 0)
        return -1;
#ifdef _WIN32
    {
        HANDLE h = CreateFileA(path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE)
            return -1;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz)) {
            CloseHandle(h);
            return -1;
        }
        if (sz.QuadPart <= 0 || (uint64_t)sz.QuadPart >= (uint64_t)cap) {
            CloseHandle(h);
            return -1;
        }
        DWORD rd = 0;
        if (!ReadFile(h, buf, (DWORD)sz.QuadPart, &rd, NULL)) {
            CloseHandle(h);
            return -1;
        }
        CloseHandle(h);
        buf[rd] = 0;
        *out_len = rd;
        return 0;
    }
#else
    {
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size >= cap)
            return -1;
        FILE *f = fopen(path, "rb");
        if (!f)
            return -1;
        size_t rd = fread(buf, 1, (size_t)st.st_size, f);
        fclose(f);
        if (rd != (size_t)st.st_size)
            return -1;
        buf[rd] = 0;
        *out_len = rd;
        return 0;
    }
#endif
}

/* 点分十进制 → 网络字节序 uint32；格式不合法返回 0 且 *ok=0 */
static inline uint32_t sim_util_ipv4_parse(const char *s, int *ok)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (ok)
        *ok = 0;
    if (!s)
        return 0;
    int n = sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d);
    if (n != 4)
        return 0;
    /* 拒绝尾部杂质：只允许数字和点 */
    {
        const char *p = s;
        while (*p) {
            if (!((*p >= '0' && *p <= '9') || *p == '.'))
                return 0;
            p++;
        }
    }
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    if (ok)
        *ok = 1;
    return (uint32_t)((d << 24) | (c << 16) | (b << 8) | a);
}

#endif
