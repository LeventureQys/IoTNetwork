#include "fake_backend.h"
#include "device_platform.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void fake_sleep_ms(int ms)
{
    Sleep((DWORD)(ms > 0 ? ms : 0));
}
#else
#include <time.h>
static void fake_sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = (ms > 0 ? ms : 0) / 1000;
    ts.tv_nsec = (long)((ms > 0 ? ms : 0) % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

typedef struct fake_backend {
    char nvs_file[1024];
    char target_ssid[33];
    char target_password[64];
    uint32_t rand_state;
} fake_backend_t;

static struct fake_globals {
    int auth_fail;
    int no_ap;
    int announce;
    int init_fail;
    int slow_recv_ms;
    int ap_started;
    int sta_connected;
    int deinit_calls;
    char last_inject[64];
} g;

void fake_backend_reset(void)
{
    memset(&g, 0, sizeof(g));
}

void fake_backend_set_auth_fail(int on) { g.auth_fail = on; }
void fake_backend_set_no_ap(int on) { g.no_ap = on; }
void fake_backend_set_announce(int on) { g.announce = on; }
void fake_backend_set_init_fail(int on) { g.init_fail = on; }
void fake_backend_set_slow_recv(int ms) { g.slow_recv_ms = ms > 0 ? ms : 0; }
int fake_backend_ap_started(void) { return g.ap_started; }
int fake_backend_sta_connected(void) { return g.sta_connected; }
int fake_backend_deinit_calls(void) { return g.deinit_calls; }
const char *fake_backend_last_inject(void) { return g.last_inject; }

static uint32_t ip_net(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

/* ---------------- NVS（文件 JSON 映射） ---------------- */

static cJSON *fake_nvs_root(fake_backend_t *fb)
{
    FILE *f = fopen(fb->nvs_file, "rb");
    if (f == NULL)
        return cJSON_CreateObject();
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return cJSON_CreateObject();
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return cJSON_CreateObject();
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root != NULL ? root : cJSON_CreateObject();
}

static int fake_nvs_save(fake_backend_t *fb, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    if (s == NULL)
        return DEMO_ERR_NOMEM;
    FILE *f = fopen(fb->nvs_file, "wb");
    int rc = DEMO_ERR;
    if (f != NULL) {
        if (fputs(s, f) >= 0)
            rc = DEMO_OK;
        fclose(f);
    }
    free(s);
    return rc;
}

static int fake_nvs_get(fake_backend_t *fb, const char *key, uint8_t *buf, int *len)
{
    cJSON *root = fake_nvs_root(fb);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    int rc = DEMO_ERR;
    if (cJSON_IsString(v)) {
        int n = (int)strlen(v->valuestring);
        if (n > *len)
            n = *len;
        if (n > 0)
            memcpy(buf, v->valuestring, (size_t)n);
        *len = (int)strlen(v->valuestring);
        rc = DEMO_OK;
    } else {
        *len = 0;
    }
    cJSON_Delete(root);
    return rc;
}

static int fake_nvs_set(fake_backend_t *fb, const char *key, const uint8_t *buf, int len)
{
    char tmp[4096 + 1];
    if (len > 4096)
        len = 4096;
    memcpy(tmp, buf, (size_t)len);
    tmp[len] = '\0';
    cJSON *root = fake_nvs_root(fb);
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddStringToObject(root, key, tmp);
    int rc = fake_nvs_save(fb, root);
    cJSON_Delete(root);
    return rc;
}

static int fake_nvs_erase(fake_backend_t *fb, const char *key)
{
    cJSON *root = fake_nvs_root(fb);
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    int rc = fake_nvs_save(fb, root);
    cJSON_Delete(root);
    return rc;
}

/* ---------------- vtable ---------------- */

static int fb_init(void *user, const char *config_path)
{
    (void)config_path;
    if (g.init_fail)
        return DEMO_ERR;
    return DEMO_OK;
}

static void fb_deinit(void *user)
{
    (void)user;
    g.deinit_calls++;
}

static int fb_wifi_scan(void *user, net_ap_info_t *aps, int *count)
{
    (void)user;
    if (aps == NULL || count == NULL)
        return DEMO_ERR_INVAL;
    *count = 0;
    return DEMO_OK;
}

static int fb_wifi_sta_connect(void *user, const char *ssid, const char *pass,
                               wifi_reason_t *reason)
{
    fake_backend_t *fb = (fake_backend_t *)user;
    if (reason == NULL || ssid == NULL || pass == NULL)
        return DEMO_ERR_INVAL;
    if (g.auth_fail) {
        *reason = WIFI_REASON_AUTH_FAIL;
        return DEMO_ERR;
    }
    if (g.no_ap || strcmp(ssid, fb->target_ssid) != 0) {
        *reason = WIFI_REASON_NO_AP_FOUND;
        return DEMO_ERR;
    }
    if (strcmp(pass, fb->target_password) != 0) {
        *reason = WIFI_REASON_AUTH_FAIL;
        return DEMO_ERR;
    }
    g.sta_connected = 1;
    *reason = WIFI_REASON_OK;
    return DEMO_OK;
}

static int fb_wifi_sta_disconnect(void *user)
{
    (void)user;
    g.sta_connected = 0;
    return DEMO_OK;
}

static int fb_wifi_ap_start(void *user, const char *ssid, const char *pass,
                            const char *pin)
{
    (void)user;
    (void)ssid;
    (void)pass;
    (void)pin;
    g.ap_started = 1;
    return DEMO_OK;
}

static int fb_wifi_ap_stop(void *user)
{
    (void)user;
    g.ap_started = 0;
    return DEMO_OK;
}

static int fb_wifi_get_rssi(void *user, int *rssi)
{
    (void)user;
    if (rssi == NULL)
        return DEMO_ERR_INVAL;
    *rssi = -50;
    return DEMO_OK;
}

static int fb_wifi_get_ip(void *user, uint32_t *ip)
{
    (void)user;
    if (ip == NULL)
        return DEMO_ERR_INVAL;
    *ip = ip_net(192, 168, 1, 50);
    return DEMO_OK;
}

static int fb_wifi_get_current_ssid(void *user, char *ssid, int capacity)
{
    fake_backend_t *fb = (fake_backend_t *)user;
    if (ssid == NULL || capacity <= 0)
        return DEMO_ERR_INVAL;
    ssid[0] = '\0';
    if (g.sta_connected)
        snprintf(ssid, (size_t)capacity, "%s", fb->target_ssid);
    return DEMO_OK;
}

static int fb_wifi_get_gateway(void *user, uint32_t *ip)
{
    (void)user;
    if (ip == NULL)
        return DEMO_ERR_INVAL;
    *ip = 0;
    return DEMO_OK;
}

static int fb_tcp_listen(void *user, uint16_t port, void **sock)
{
    (void)user;
    (void)port;
    if (sock == NULL)
        return DEMO_ERR_INVAL;
    *sock = (void *)(uintptr_t)1;
    return DEMO_OK;
}

static int fb_tcp_accept(void *user, void *listen, void **conn, net_addr_t *peer)
{
    (void)user;
    (void)listen;
    (void)peer;
    if (conn == NULL)
        return DEMO_ERR_INVAL;
    *conn = NULL;
    return DEMO_ERR_AGAIN;
}

static int fb_tcp_connect(void *user, const net_addr_t *addr, void **sock,
                          int timeout_ms)
{
    (void)user;
    (void)addr;
    (void)timeout_ms;
    if (sock == NULL)
        return DEMO_ERR_INVAL;
    if (!g.announce)
        return DEMO_ERR;
    *sock = (void *)(uintptr_t)2;
    return DEMO_OK;
}

static int fb_sock_send(void *user, void *sock, const uint8_t *buf, int len)
{
    (void)user;
    (void)sock;
    (void)buf;
    return len;
}

static int fb_sock_recv(void *user, void *sock, uint8_t *buf, int cap)
{
    (void)user;
    (void)sock;
    (void)buf;
    (void)cap;
    if (g.slow_recv_ms > 0)
        fake_sleep_ms(g.slow_recv_ms);
    return DEMO_ERR_AGAIN;
}

static void fb_sock_close(void *user, void *sock)
{
    (void)user;
    (void)sock;
}

static int fb_udp_mcast_join(void *user, const char *group, uint16_t port, void **sock)
{
    (void)user;
    (void)group;
    (void)port;
    if (sock == NULL)
        return DEMO_ERR_INVAL;
    *sock = (void *)(uintptr_t)3;
    return DEMO_OK;
}

static int fb_udp_send(void *user, const char *group, uint16_t port, const uint8_t *buf,
                       int len)
{
    (void)user;
    (void)group;
    (void)port;
    (void)buf;
    return len;
}

static int fb_udp_recv(void *user, void *sock, uint8_t *buf, int cap, net_addr_t *from)
{
    (void)user;
    (void)sock;
    (void)buf;
    (void)cap;
    (void)from;
    return DEMO_ERR_AGAIN;
}

static int fb_mdns_register(void *user, const net_mdns_service_t *svc)
{
    (void)user;
    (void)svc;
    return DEMO_OK;
}

static int fb_mdns_unregister(void *user, const char *type)
{
    (void)user;
    (void)type;
    return DEMO_OK;
}

static int fb_mdns_resolve(void *user, const char *type, net_mdns_service_t *out,
                           int timeout_ms)
{
    (void)user;
    (void)type;
    (void)timeout_ms;
    if (out == NULL)
        return DEMO_ERR_INVAL;
    if (!g.announce)
        return DEMO_ERR;
    memset(out, 0, sizeof(*out));
    snprintf(out->instance, sizeof(out->instance), "%s", "fake-host");
    out->addr.ip = ip_net(127, 0, 0, 1);
    out->addr.port = (uint16_t)((5935u >> 8) | (5935u << 8));
    return DEMO_OK;
}

static int fb_nvs_get(void *user, const char *key, uint8_t *buf, int *len)
{
    return fake_nvs_get((fake_backend_t *)user, key, buf, len);
}

static int fb_nvs_set(void *user, const char *key, const uint8_t *buf, int len)
{
    return fake_nvs_set((fake_backend_t *)user, key, buf, len);
}

static int fb_nvs_erase(void *user, const char *key)
{
    return fake_nvs_erase((fake_backend_t *)user, key);
}

static uint64_t fb_time_ms(void *user)
{
    (void)user;
    return device_platform_default.monotonic_ms();
}

static uint32_t fb_random(void *user)
{
    fake_backend_t *fb = (fake_backend_t *)user;
    fb->rand_state = fb->rand_state * 1664525u + 1013904223u;
    return fb->rand_state;
}

static int fb_inject(void *user, const char *action, const char *arg_json)
{
    (void)user;
    (void)arg_json;
    if (action == NULL)
        return DEMO_ERR_INVAL;
    snprintf(g.last_inject, sizeof(g.last_inject), "%s", action);
    return DEMO_OK;
}

static const net_backend_t fake_vtable = {
    fb_init,
    fb_deinit,
    fb_wifi_scan,
    fb_wifi_sta_connect,
    fb_wifi_sta_disconnect,
    fb_wifi_ap_start,
    fb_wifi_ap_stop,
    fb_wifi_get_rssi,
    fb_wifi_get_ip,
    fb_wifi_get_current_ssid,
    fb_wifi_get_gateway,
    fb_tcp_listen,
    fb_tcp_accept,
    fb_tcp_connect,
    fb_sock_send,
    fb_sock_recv,
    fb_sock_close,
    fb_udp_mcast_join,
    fb_udp_send,
    fb_udp_recv,
    fb_mdns_register,
    fb_mdns_unregister,
    fb_mdns_resolve,
    fb_nvs_get,
    fb_nvs_set,
    fb_nvs_erase,
    fb_time_ms,
    fb_random,
    fb_inject
};

const net_backend_t *fake_backend_vtable(void)
{
    return &fake_vtable;
}

static void fake_destroy_user(void *user)
{
    free(user);
}

device_result_t device_sim_backend_create(
    const device_sim_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error)
{
    if (out_instance != NULL)
        memset(out_instance, 0, sizeof(*out_instance));
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (options == NULL || out_instance == NULL) {
        if (error != NULL) {
            error->code = DEVICE_ERR_INVALID_ARGUMENT;
            snprintf(error->operation, sizeof(error->operation), "%s", "sim_backend");
        }
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    fake_backend_t *fb = (fake_backend_t *)calloc(1, sizeof(fake_backend_t));
    if (fb == NULL) {
        if (error != NULL) {
            error->code = DEVICE_ERR_NO_MEMORY;
            snprintf(error->operation, sizeof(error->operation), "%s", "sim_backend");
        }
        return DEVICE_ERR_NO_MEMORY;
    }
    snprintf(fb->nvs_file, sizeof(fb->nvs_file), "%s",
             (options->nvs_file != NULL && options->nvs_file[0] != '\0')
                 ? options->nvs_file
                 : "run/test.nvs.json");
    snprintf(fb->target_ssid, sizeof(fb->target_ssid), "%s",
             (options->target_ssid != NULL && options->target_ssid[0] != '\0')
                 ? options->target_ssid
                 : "TactileFactory-2.4G");
    snprintf(fb->target_password, sizeof(fb->target_password), "%s",
             (options->target_password != NULL && options->target_password[0] != '\0')
                 ? options->target_password
                 : "securepass123");
    fb->rand_state = (uint32_t)options->device_index * 2654435761u + 12345u;
    out_instance->vtable = &fake_vtable;
    out_instance->user = fb;
    out_instance->destroy_user = fake_destroy_user;
    return DEVICE_OK;
}

device_result_t device_linux_backend_create(
    const device_linux_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error)
{
    (void)options;
    if (out_instance != NULL)
        memset(out_instance, 0, sizeof(*out_instance));
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->code = DEVICE_ERR_NOT_SUPPORTED;
        snprintf(error->operation, sizeof(error->operation), "%s", "linux_backend");
        snprintf(error->message, sizeof(error->message), "%s", "非 Linux 平台不支持");
    }
    return DEVICE_ERR_NOT_SUPPORTED;
}
