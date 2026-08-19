#ifndef DEMO_DEVICE_DEVICE_ATOMIC_H
#define DEMO_DEVICE_DEVICE_ATOMIC_H

/* 跨线程标志（stop 等）的最小同步原语：
 * Windows 使用 InterlockedExchange（MSVC 不依赖 /std:c11 /experimental:c11atomics），
 * POSIX 使用 GCC/Clang __atomic 内建。禁止用裸 volatile 替代。 */

#ifdef _WIN32
#include <windows.h>
typedef volatile LONG dev_atomic_int_t;
static inline void dev_atomic_set(dev_atomic_int_t *p, long v)
{
    InterlockedExchange(p, v);
}
static inline long dev_atomic_get(const dev_atomic_int_t *p)
{
    return InterlockedCompareExchange((dev_atomic_int_t *)p, 0, 0);
}
#else
typedef volatile int dev_atomic_int_t;
static inline void dev_atomic_set(dev_atomic_int_t *p, int v)
{
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline int dev_atomic_get(const dev_atomic_int_t *p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
#endif

#endif
