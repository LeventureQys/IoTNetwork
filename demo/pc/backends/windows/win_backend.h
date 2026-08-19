#ifndef PC_WIN_BACKEND_H
#define PC_WIN_BACKEND_H

#include "net_abstraction.h"
#include "params.h"

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

void *win_backend_create(const demo_params_t *params);
void win_backend_destroy(void *user);
const net_backend_t *win_backend_table(void);
int win_backend_get_ipv4(char *buffer, int capacity);
int win_backend_ipv4_valid(const char *ip); /* 拒绝 unspecified/loopback/APIPA/非法格式 */

#ifdef __cplusplus
}
#endif

#else /* !_WIN32：真实 WiFi 配网后端仅 Windows 支持，Linux 下提供空桩 */

static inline void *win_backend_create(const demo_params_t *params)
{
    (void)params;
    return nullptr;
}

static inline void win_backend_destroy(void *user) { (void)user; }

static inline const net_backend_t *win_backend_table(void) { return nullptr; }

static inline int win_backend_get_ipv4(char *buffer, int capacity)
{
    (void)buffer;
    (void)capacity;
    return DEMO_ERR;
}

static inline int win_backend_ipv4_valid(const char *ip)
{
    (void)ip;
    return DEMO_ERR;
}

#endif /* _WIN32 */

#endif
