#include "device_platform.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct device_thread {
    pthread_t tid;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int joined;
};

struct device_mutex {
    pthread_mutex_t mutex;
};

struct device_cond {
    pthread_cond_t cond;
};

struct thread_param {
    void (*entry)(void *user);
    void *arg;
    struct device_thread *thread;
};

static void *thread_entry_posix(void *user)
{
    struct thread_param *p = (struct thread_param *)user;
    struct device_thread *t = p->thread;
    p->entry(p->arg);
    pthread_mutex_lock(&t->mutex);
    t->done = 1;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->mutex);
    free(p);
    return NULL;
}

static device_thread_t *plat_thread_create(void (*entry)(void *user), void *user)
{
    device_thread_t *t = (device_thread_t *)calloc(1, sizeof(device_thread_t));
    if (t == NULL)
        return NULL;
    if (pthread_mutex_init(&t->mutex, NULL) != 0) {
        free(t);
        return NULL;
    }
    if (pthread_cond_init(&t->cond, NULL) != 0) {
        pthread_mutex_destroy(&t->mutex);
        free(t);
        return NULL;
    }
    struct thread_param *p = (struct thread_param *)malloc(sizeof(struct thread_param));
    if (p == NULL) {
        pthread_cond_destroy(&t->cond);
        pthread_mutex_destroy(&t->mutex);
        free(t);
        return NULL;
    }
    p->entry = entry;
    p->arg = user;
    p->thread = t;
    if (pthread_create(&t->tid, NULL, thread_entry_posix, p) != 0) {
        free(p);
        pthread_cond_destroy(&t->cond);
        pthread_mutex_destroy(&t->mutex);
        free(t);
        return NULL;
    }
    return t;
}

static int plat_thread_join(device_thread_t *t, uint32_t timeout_ms)
{
    if (t == NULL)
        return DEVICE_PLAT_OK;
    pthread_mutex_lock(&t->mutex);
    if (!t->done) {
        if (timeout_ms == UINT32_MAX) {
            while (!t->done)
                pthread_cond_wait(&t->cond, &t->mutex);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeout_ms / 1000;
            ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            while (!t->done) {
                if (pthread_cond_timedwait(&t->cond, &t->mutex, &ts) == ETIMEDOUT)
                    break;
            }
        }
    }
    if (!t->done) {
        pthread_mutex_unlock(&t->mutex);
        return DEVICE_PLAT_TIMEOUT;
    }
    if (!t->joined) {
        t->joined = 1;
        pthread_mutex_unlock(&t->mutex);
        pthread_join(t->tid, NULL);
        return DEVICE_PLAT_OK;
    }
    pthread_mutex_unlock(&t->mutex);
    return DEVICE_PLAT_OK;
}

static void plat_thread_destroy(device_thread_t *t)
{
    if (t == NULL)
        return;
    if (!t->joined) {
        pthread_mutex_lock(&t->mutex);
        int done = t->done;
        pthread_mutex_unlock(&t->mutex);
        if (!done)
            pthread_join(t->tid, NULL);
    }
    pthread_cond_destroy(&t->cond);
    pthread_mutex_destroy(&t->mutex);
    free(t);
}

static device_mutex_t *plat_mutex_create(void)
{
    device_mutex_t *m = (device_mutex_t *)malloc(sizeof(device_mutex_t));
    if (m == NULL)
        return NULL;
    if (pthread_mutex_init(&m->mutex, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

static void plat_mutex_destroy(device_mutex_t *m)
{
    if (m == NULL)
        return;
    pthread_mutex_destroy(&m->mutex);
    free(m);
}

static void plat_mutex_lock(device_mutex_t *m)
{
    if (m == NULL)
        return;
    pthread_mutex_lock(&m->mutex);
}

static void plat_mutex_unlock(device_mutex_t *m)
{
    if (m == NULL)
        return;
    pthread_mutex_unlock(&m->mutex);
}

static device_cond_t *plat_cond_create(void)
{
    device_cond_t *c = (device_cond_t *)malloc(sizeof(device_cond_t));
    if (c == NULL)
        return NULL;
    if (pthread_cond_init(&c->cond, NULL) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

static void plat_cond_destroy(device_cond_t *c)
{
    if (c == NULL)
        return;
    pthread_cond_destroy(&c->cond);
    free(c);
}

static void plat_cond_wait(device_cond_t *c, device_mutex_t *m)
{
    if (c == NULL || m == NULL)
        return;
    pthread_cond_wait(&c->cond, &m->mutex);
}

static int plat_cond_wait_timeout(device_cond_t *c, device_mutex_t *m, uint32_t timeout_ms)
{
    if (c == NULL || m == NULL)
        return DEVICE_PLAT_TIMEOUT;
    if (timeout_ms == UINT32_MAX) {
        pthread_cond_wait(&c->cond, &m->mutex);
        return DEVICE_PLAT_OK;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(&c->cond, &m->mutex, &ts) == ETIMEDOUT
               ? DEVICE_PLAT_TIMEOUT
               : DEVICE_PLAT_OK;
}

static void plat_cond_signal(device_cond_t *c)
{
    if (c == NULL)
        return;
    pthread_cond_signal(&c->cond);
}

static void plat_cond_broadcast(device_cond_t *c)
{
    if (c == NULL)
        return;
    pthread_cond_broadcast(&c->cond);
}

static uint64_t plat_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

const device_platform_t device_platform_default = {
    plat_thread_create,
    plat_thread_join,
    plat_thread_destroy,
    plat_mutex_create,
    plat_mutex_destroy,
    plat_mutex_lock,
    plat_mutex_unlock,
    plat_cond_create,
    plat_cond_destroy,
    plat_cond_wait,
    plat_cond_wait_timeout,
    plat_cond_signal,
    plat_cond_broadcast,
    plat_monotonic_ms
};
