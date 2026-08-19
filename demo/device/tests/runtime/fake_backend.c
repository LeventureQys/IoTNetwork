#include "fake_backend.h"
#include "device_platform.h"
#include "frame.h"
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

/* beta v1.1 固定契约（与 include/protocol.h 一致） */
#define FAKE_TARGET_SSID     PROTO_PC_AP_DEFAULT_SSID
#define FAKE_TARGET_PASSWORD PROTO_PC_AP_PASSWORD
#define FAKE_SIMILAR_SSID    "Modu_PCX"

typedef struct fake_backend {
    char nvs_file[1024];
    uint32_t rand_state;
} fake_backend_t;

static struct fake_globals {
    int auth_fail;
    int no_ap;
    int similar_ssid;
    int init_fail;
    int slow_recv_ms;
    int dhcp_fail;
    int tcp_fail;
    int wifi_drop;
    int ack_ok;
    int ack_busy;
    int ack_sent;
    int sta_connected;
    int sta_disconnect_calls;
    int deinit_calls;
    int ap_start_calls;
    int mdns_calls;
    int udp_calls;
    int tcp_listen_calls;
    uint32_t last_connect_ip;
    uint16_t last_connect_port;
    char last_inject[64];
} g;

void fake_backend_reset(void)
{
    memset(&g, 0, sizeof(g));
}

void fake_backend_set_auth_fail(int on) { g.auth_fail = on; }
void fake_backend_set_no_ap(int on) { g.no_ap = on; }
void fake_backend_set_similar_ssid(int on) { g.similar_ssid = on; }
void fake_backend_set_init_fail(int on) { g.init_fail = on; }
void fake_backend_set_slow_recv(int ms) { g.slow_recv_ms = ms > 0 ? ms : 0; }
void fake_backend_set_dhcp_fail(int on) { g.dhcp_fail = on; }
void fake_backend_set_tcp_fail(int on) { g.tcp_fail = on; }
void fake_backend_set_wifi_drop(int on) { g.wifi_drop = on; }
void fake_backend_set_ack_ok(int on) { g.ack_ok = on; }
void fake_backend_set_ack_busy(int on) { g.ack_busy = on; }

int fake_backend_sta_connected(void) { return g.sta_connected; }
int fake_backend_sta_disconnect_calls(void) { return g.sta_disconnect_calls; }
int fake_backend_deinit_calls(void) { return g.deinit_calls; }
int fake_backend_ap_start_calls(void) { return g.ap_start_calls; }
int fake_backend_mdns_calls(void) { return g.mdns_calls; }
int fake_backend_udp_calls(void) { return g.udp_calls; }
int fake_backend_tcp_listen_calls(void) { return g.tcp_listen_calls; }
uint32_t fake_backend_last_connect_ip(void) { return g.last_connect_ip; }
uint16_t fake_backend_last_connect_port(void) { return g.last_connect_port; }
const char *fake_backend_last_inject(void) { return g.last_inject; }

static uint32_t ip_net(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    /* 与 net_abstraction/sim_world/linux_wifi 一致：内存字节序 = 点分四段顺序 */
    return ((uint32_t)d << 24) | ((uint32_t)c << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

/* ---------------- NVS（文件 JSON 映射，事件日志持久化用） ---------------- */

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
    if (g.no_ap) {
        *count = 0;
        return DEMO_OK;
    }
    if (*count <= 0)
        return DEMO_ERR_INVAL;
    memset(&aps[0], 0, sizeof(aps[0]));
    snprintf(aps[0].ssid, sizeof(aps[0].ssid), "%s",
             g.similar_ssid ? FAKE_SIMILAR_SSID : FAKE_TARGET_SSID);
    aps[0].rssi = -45;
    aps[0].band_2g = 1;
    *count = 1;
    return DEMO_OK;
}

static int fb_wifi_sta_connect(void *user, const char *ssid, const char *pass,
                               wifi_reason_t *reason)
{
    (void)user;
    if (reason == NULL || ssid == NULL || pass == NULL)
        return DEMO_ERR_INVAL;
    if (g.auth_fail) {
        *reason = WIFI_REASON_AUTH_FAIL;
        return DEMO_ERR;
    }
    if (g.no_ap || strcmp(ssid, FAKE_TARGET_SSID) != 0) {
        *reason = WIFI_REASON_NO_AP_FOUND;
        return DEMO_ERR;
    }
    if (strcmp(pass, FAKE_TARGET_PASSWORD) != 0) {
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
    g.sta_disconnect_calls++;
    return DEMO_OK;
}

static int fb_wifi_ap_start(void *user, const char *ssid, const char *pass,
                            const char *pin)
{
    (void)user;
    (void)ssid;
    (void)pass;
    (void)pin;
    g.ap_start_calls++;
    return DEMO_OK;
}

static int fb_wifi_ap_stop(void *user)
{
    (void)user;
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
    if (g.wifi_drop) {
        *ip = 0;
        return DEMO_ERR;
    }
    if (g.dhcp_fail) {
        *ip = 0; /* 已连接但 DHCP 未获得地址 */
        return DEMO_OK;
    }
    *ip = ip_net(192, 168, 137, 50);
    return DEMO_OK;
}

static int fb_wifi_get_current_ssid(void *user, char *ssid, int capacity)
{
    (void)user;
    if (ssid == NULL || capacity <= 0)
        return DEMO_ERR_INVAL;
    ssid[0] = '\0';
    if (g.sta_connected && !g.wifi_drop)
        snprintf(ssid, (size_t)capacity, "%s", FAKE_TARGET_SSID);
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
    g.tcp_listen_calls++;
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
    (void)timeout_ms;
    if (sock == NULL || addr == NULL)
        return DEMO_ERR_INVAL;
    g.last_connect_ip = addr->ip;   /* 无论成败均记录被请求的目标地址 */
    g.last_connect_port = addr->port;
    g.ack_sent = 0;
    if (g.tcp_fail)
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
    if (g.slow_recv_ms > 0)
        fake_sleep_ms(g.slow_recv_ms);
    if (buf == NULL || cap <= 0)
        return DEMO_ERR_INVAL;

    /* 一次性下发 host_ack（ok 或 busy），随后无数据（供心跳超时用例） */
    if (!g.ack_sent && (g.ack_ok || g.ack_busy)) {
        const char *json = g.ack_busy
            ? "{\"cmd\":\"host_ack\",\"status\":\"busy\",\"reason\":\"single_device_only\"}"
            : "{\"cmd\":\"host_ack\",\"status\":\"ok\",\"heartbeat_interval\":1,"
              "\"session_id\":\"deadbeef\",\"proto_ver\":1}";
        uint8_t frame[512];
        int flen = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, 1,
                                  (const uint8_t *)json, (int)strlen(json),
                                  frame, (int)sizeof(frame));
        if (flen > 0 && flen <= cap) {
            memcpy(buf, frame, (size_t)flen);
            g.ack_sent = 1;
            return flen;
        }
    }
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
    g.udp_calls++;
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
    g.udp_calls++;
    return len;
}

static int fb_udp_recv(void *user, void *sock, uint8_t *buf, int cap, net_addr_t *from)
{
    (void)user;
    (void)sock;
    (void)buf;
    (void)cap;
    (void)from;
    g.udp_calls++;
    return DEMO_ERR_AGAIN;
}

static int fb_mdns_register(void *user, const net_mdns_service_t *svc)
{
    (void)user;
    (void)svc;
    g.mdns_calls++;
    return DEMO_OK;
}

static int fb_mdns_unregister(void *user, const char *type)
{
    (void)user;
    (void)type;
    g.mdns_calls++;
    return DEMO_OK;
}

static int fb_mdns_resolve(void *user, const char *type, net_mdns_service_t *out,
                           int timeout_ms)
{
    (void)user;
    (void)type;
    (void)out;
    (void)timeout_ms;
    g.mdns_calls++;
    return DEMO_ERR;
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
