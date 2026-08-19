/* ============================================================================
 * sim_world.c - 模拟网络世界（显式实例 + 内部锁）。
 *
 * 迁移自旧 net_sim/sim_world.cpp：去除 singleton 与 C++ 容器/锁，
 * 保持全部状态机语义（AP 注册/注销、STA 连接判定、RSSI、mDNS、组播屏蔽、
 * 虚拟 IP 推导）。所有接口线程安全。
 * ========================================================================== */
#include "sim_world.h"
#include "sim_util.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef struct sim_rssi_entry {
    char tag[16];
    int rssi;
} sim_rssi_entry_t;

typedef struct sim_block_entry {
    char tag[16];
    int blocked;
} sim_block_entry_t;

struct sim_world {
    void *lock; /* Windows: SRWLOCK*；POSIX: pthread_mutex_t* */

    char target_ssid[33];
    char target_password[64];
    int target_band_2g;
    int target_up;
    int target_auth_fail;

    sim_ap_t aps[SIM_WORLD_MAX_APS];
    int ap_count;

    sim_rssi_entry_t rssi[SIM_WORLD_MAX_RSSI];
    int rssi_count;

    sim_mdns_svc_t mdns[SIM_WORLD_MAX_MDNS];
    int mdns_count;

    sim_block_entry_t blocked[SIM_WORLD_MAX_BLOCKED];
    int blocked_count;
};

static void world_lock(sim_world_t *w)
{
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)w->lock);
#else
    pthread_mutex_lock((pthread_mutex_t *)w->lock);
#endif
}

static void world_unlock(sim_world_t *w)
{
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)w->lock);
#else
    pthread_mutex_unlock((pthread_mutex_t *)w->lock);
#endif
}

sim_world_t *sim_world_create(void)
{
    sim_world_t *w = (sim_world_t *)calloc(1, sizeof(sim_world_t));
    if (!w)
        return NULL;
#ifdef _WIN32
    CRITICAL_SECTION *lk = (CRITICAL_SECTION *)calloc(1, sizeof(CRITICAL_SECTION));
    if (!lk) {
        free(w);
        return NULL;
    }
    InitializeCriticalSection(lk);
    w->lock = lk;
#else
    pthread_mutex_t *lk = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
    if (!lk) {
        free(w);
        return NULL;
    }
    if (pthread_mutex_init(lk, NULL) != 0) {
        free(lk);
        free(w);
        return NULL;
    }
    w->lock = lk;
#endif
    w->target_band_2g = 1;
    w->target_up = 1;
    return w;
}

void sim_world_destroy(sim_world_t *w)
{
    if (!w)
        return;
#ifdef _WIN32
    if (w->lock) {
        DeleteCriticalSection((CRITICAL_SECTION *)w->lock);
        free(w->lock);
    }
#else
    if (w->lock) {
        pthread_mutex_destroy((pthread_mutex_t *)w->lock);
        free(w->lock);
    }
#endif
    free(w);
}

/* ---------------- 目标网络 ---------------- */

void sim_world_target_set(sim_world_t *w, const char *ssid, const char *pass, int band_2g)
{
    if (!w)
        return;
    world_lock(w);
    sim_util_copy_bounded(w->target_ssid, sizeof(w->target_ssid), ssid);
    sim_util_copy_bounded(w->target_password, sizeof(w->target_password), pass);
    w->target_band_2g = band_2g ? 1 : 0;
    w->target_up = 1;
    w->target_auth_fail = 0;
    world_unlock(w);
}

void sim_world_target_set_up(sim_world_t *w, int up)
{
    if (!w)
        return;
    world_lock(w);
    w->target_up = up ? 1 : 0;
    world_unlock(w);
}

int sim_world_target_up(const sim_world_t *w)
{
    int v = 0;
    if (!w)
        return 0;
    world_lock((sim_world_t *)w);
    v = w->target_up;
    world_unlock((sim_world_t *)w);
    return v;
}

void sim_world_target_set_auth_fail(sim_world_t *w, int on)
{
    if (!w)
        return;
    world_lock(w);
    w->target_auth_fail = on ? 1 : 0;
    world_unlock(w);
}

int sim_world_target_auth_fail(const sim_world_t *w)
{
    int v = 0;
    if (!w)
        return 0;
    world_lock((sim_world_t *)w);
    v = w->target_auth_fail;
    world_unlock((sim_world_t *)w);
    return v;
}

const char *sim_world_target_ssid(const sim_world_t *w)
{
    if (!w)
        return "";
    world_lock((sim_world_t *)w);
    const char *p = w->target_ssid;
    world_unlock((sim_world_t *)w);
    return p;
}

const char *sim_world_target_password(const sim_world_t *w)
{
    if (!w)
        return "";
    world_lock((sim_world_t *)w);
    const char *p = w->target_password;
    world_unlock((sim_world_t *)w);
    return p;
}

int sim_world_target_band_2g(const sim_world_t *w)
{
    int v = 1;
    if (!w)
        return 1;
    world_lock((sim_world_t *)w);
    v = w->target_band_2g;
    world_unlock((sim_world_t *)w);
    return v;
}

/* ---------------- 虚拟 AP ---------------- */

void sim_world_ap_register(sim_world_t *w, const char *owner_tag, const char *ssid,
                           const char *pass, const char *pin, uint16_t real_port)
{
    if (!w)
        return;
    world_lock(w);
    int i;
    for (i = 0; i < w->ap_count; i++) {
        if (strcmp(w->aps[i].owner_tag, owner_tag) == 0) {
            memset(&w->aps[i], 0, sizeof(sim_ap_t));
            sim_util_copy_bounded(w->aps[i].ssid, sizeof(w->aps[i].ssid), ssid);
            sim_util_copy_bounded(w->aps[i].password, sizeof(w->aps[i].password), pass);
            sim_util_copy_bounded(w->aps[i].pin, sizeof(w->aps[i].pin), pin);
            sim_util_copy_bounded(w->aps[i].owner_tag, sizeof(w->aps[i].owner_tag), owner_tag);
            w->aps[i].real_port = real_port;
            w->aps[i].active = 1;
            world_unlock(w);
            return;
        }
    }
    if (w->ap_count < SIM_WORLD_MAX_APS) {
        sim_ap_t *ap = &w->aps[w->ap_count++];
        memset(ap, 0, sizeof(*ap));
        sim_util_copy_bounded(ap->ssid, sizeof(ap->ssid), ssid);
        sim_util_copy_bounded(ap->password, sizeof(ap->password), pass);
        sim_util_copy_bounded(ap->pin, sizeof(ap->pin), pin);
        sim_util_copy_bounded(ap->owner_tag, sizeof(ap->owner_tag), owner_tag);
        ap->real_port = real_port;
        ap->active = 1;
    }
    world_unlock(w);
}

void sim_world_ap_unregister(sim_world_t *w, const char *owner_tag)
{
    if (!w)
        return;
    world_lock(w);
    int i;
    for (i = 0; i < w->ap_count; i++) {
        if (strcmp(w->aps[i].owner_tag, owner_tag) == 0) {
            memset(&w->aps[i], 0, sizeof(sim_ap_t));
            w->aps[i] = w->aps[w->ap_count - 1];
            memset(&w->aps[w->ap_count - 1], 0, sizeof(sim_ap_t));
            w->ap_count--;
            break;
        }
    }
    world_unlock(w);
}

int sim_world_ap_find(const sim_world_t *w, const char *ssid, sim_ap_t *out)
{
    if (!w || !ssid)
        return 0;
    world_lock((sim_world_t *)w);
    int found = 0;
    int i;
    for (i = 0; i < w->ap_count; i++) {
        if (w->aps[i].active && strcmp(w->aps[i].ssid, ssid) == 0) {
            if (out)
                *out = w->aps[i];
            found = 1;
            break;
        }
    }
    world_unlock((sim_world_t *)w);
    return found;
}

int sim_world_ap_count(const sim_world_t *w)
{
    if (!w)
        return 0;
    world_lock((sim_world_t *)w);
    int n = w->ap_count;
    world_unlock((sim_world_t *)w);
    return n;
}

int sim_world_ap_iterate(const sim_world_t *w, int *pos, sim_ap_t *out)
{
    if (!w || !pos || !out)
        return 0;
    world_lock((sim_world_t *)w);
    int found = 0;
    int i;
    for (i = *pos; i < w->ap_count; i++) {
        if (w->aps[i].active) {
            *out = w->aps[i];
            *pos = i + 1;
            found = 1;
            break;
        }
    }
    world_unlock((sim_world_t *)w);
    return found;
}

int sim_world_sta_connect(const sim_world_t *w, const char *ssid, const char *pass)
{
    if (!w || !ssid || !pass)
        return 201;
    world_lock((sim_world_t *)w);
    int reason = 201;
    int i;
    for (i = 0; i < w->ap_count; i++) {
        if (w->aps[i].active && strcmp(w->aps[i].ssid, ssid) == 0) {
            reason = (strcmp(w->aps[i].password, pass) == 0) ? 0 : 202;
            world_unlock((sim_world_t *)w);
            return reason;
        }
    }
    if (strcmp(ssid, w->target_ssid) != 0) {
        reason = 201;
    } else if (!w->target_up) {
        reason = 201;
    } else if (w->target_auth_fail || strcmp(pass, w->target_password) != 0) {
        reason = 202;
    } else if (!w->target_band_2g) {
        reason = 500;
    } else {
        reason = 0;
    }
    world_unlock((sim_world_t *)w);
    return reason;
}

/* ---------------- RSSI ---------------- */

void sim_world_rssi_set(sim_world_t *w, const char *tag, int rssi)
{
    if (!w || !tag)
        return;
    world_lock(w);
    int i;
    for (i = 0; i < w->rssi_count; i++) {
        if (strcmp(w->rssi[i].tag, tag) == 0) {
            w->rssi[i].rssi = rssi;
            world_unlock(w);
            return;
        }
    }
    if (w->rssi_count < SIM_WORLD_MAX_RSSI) {
        sim_util_copy_bounded(w->rssi[w->rssi_count].tag,
                              sizeof(w->rssi[w->rssi_count].tag), tag);
        w->rssi[w->rssi_count].rssi = rssi;
        w->rssi_count++;
    }
    world_unlock(w);
}

int sim_world_rssi_get(const sim_world_t *w, const char *tag)
{
    if (!w || !tag)
        return -58;
    world_lock((sim_world_t *)w);
    int rssi = -58; /* 默认 RSSI */
    int i;
    for (i = 0; i < w->rssi_count; i++) {
        if (strcmp(w->rssi[i].tag, tag) == 0) {
            rssi = w->rssi[i].rssi;
            break;
        }
    }
    world_unlock((sim_world_t *)w);
    return rssi;
}

/* ---------------- mDNS ---------------- */

void sim_world_mdns_register(sim_world_t *w, const sim_mdns_svc_t *svc)
{
    if (!w || !svc)
        return;
    world_lock(w);
    int i;
    for (i = 0; i < w->mdns_count; i++) {
        if (strcmp(w->mdns[i].type, svc->type) == 0) {
            w->mdns[i] = *svc;
            world_unlock(w);
            return;
        }
    }
    if (w->mdns_count < SIM_WORLD_MAX_MDNS)
        w->mdns[w->mdns_count++] = *svc;
    world_unlock(w);
}

void sim_world_mdns_unregister(sim_world_t *w, const char *type)
{
    if (!w || !type)
        return;
    world_lock(w);
    int i;
    for (i = 0; i < w->mdns_count; i++) {
        if (strcmp(w->mdns[i].type, type) == 0) {
            w->mdns[i] = w->mdns[w->mdns_count - 1];
            memset(&w->mdns[w->mdns_count - 1], 0, sizeof(sim_mdns_svc_t));
            w->mdns_count--;
            break;
        }
    }
    world_unlock(w);
}

int sim_world_mdns_resolve(const sim_world_t *w, const char *type, sim_mdns_svc_t *out)
{
    if (!w || !type)
        return 0;
    world_lock((sim_world_t *)w);
    int found = 0;
    int i;
    for (i = 0; i < w->mdns_count; i++) {
        if (w->mdns[i].active && strcmp(w->mdns[i].type, type) == 0) {
            if (out)
                *out = w->mdns[i];
            found = 1;
            break;
        }
    }
    world_unlock((sim_world_t *)w);
    return found;
}

/* ---------------- 组播屏蔽 ---------------- */

void sim_world_mcast_set_blocked(sim_world_t *w, const char *tag, int blocked)
{
    if (!w || !tag)
        return;
    world_lock(w);
    int i;
    for (i = 0; i < w->blocked_count; i++) {
        if (strcmp(w->blocked[i].tag, tag) == 0) {
            w->blocked[i].blocked = blocked ? 1 : 0;
            world_unlock(w);
            return;
        }
    }
    if (w->blocked_count < SIM_WORLD_MAX_BLOCKED) {
        sim_util_copy_bounded(w->blocked[w->blocked_count].tag,
                              sizeof(w->blocked[w->blocked_count].tag), tag);
        w->blocked[w->blocked_count].blocked = blocked ? 1 : 0;
        w->blocked_count++;
    }
    world_unlock(w);
}

int sim_world_mcast_is_blocked(const sim_world_t *w, const char *tag)
{
    if (!w || !tag)
        return 0;
    world_lock((sim_world_t *)w);
    int blocked = 0;
    int i;
    for (i = 0; i < w->blocked_count; i++) {
        if (strcmp(w->blocked[i].tag, tag) == 0) {
            blocked = w->blocked[i].blocked;
            break;
        }
    }
    world_unlock((sim_world_t *)w);
    return blocked;
}

/* ---------------- 虚拟 IP（语义冻结） ---------------- */

static uint32_t world_ip_of(const char *s)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    return (uint32_t)((d << 24) | (c << 16) | (b << 8) | a);
}

uint32_t sim_world_host_virtual_ip(void) { return world_ip_of("192.168.1.50"); }
uint32_t sim_world_device_ap_virtual_ip(void) { return world_ip_of(PROTO_SIM_AP_IP); }

uint32_t sim_world_device_sta_virtual_ip(int dev_index)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "192.168.1.%d", 100 + dev_index);
    return world_ip_of(buf);
}

uint16_t sim_world_device_ap_real_port(int base, int dev_index)
{
    return (uint16_t)(base + dev_index);
}
