#include "device_linux_backend.h"

#include "linux_nvs.h"
#include "linux_socket.h"
#include "linux_wifi.h"
#include "log.h"
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "LINUX_BACKEND"

#define LINUX_JSON_DEFAULT_NVS_FILE "run/device_linux.nvs.json"

typedef struct linux_backend {
    linux_wifi_t *wifi;
    linux_nvs_t *nvs;
} linux_backend_t;

/* ---------------- device_linux.json 解析（Linux 专用字段） ---------------- */

typedef struct linux_backend_config {
    int enable;
    char sta_interface[32];
    char nvs_file[384];
} linux_backend_config_t;

static void linux_backend_config_defaults(linux_backend_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->enable = 1;
    snprintf(config->nvs_file, sizeof(config->nvs_file), "%s",
             LINUX_JSON_DEFAULT_NVS_FILE);
}

/*
 * 读取 device_linux.json 的 Linux 专用字段。
 * 与设计文档第 10 节一致：文件缺失使用内置默认值并打印警告；
 * 未知或类型不匹配字段忽略并打印警告；字符串按目标容量截断。
 */
static void linux_backend_config_load(linux_backend_config_t *config,
                                      const char *path)
{
    FILE *file;
    long file_size;
    char *buffer;
    cJSON *root;
    const cJSON *item;

    if (config == NULL || path == NULL || path[0] == '\0')
        return;
    file = fopen(path, "rb");
    if (file == NULL) {
        LOG_W(TAG, "设备 Linux 配置 %s 缺失，使用内置默认值", path);
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0 || file_size == 0 ||
        (unsigned long)file_size > 1024U * 1024U) {
        LOG_W(TAG, "设备 Linux 配置 %s 不可读，使用内置默认值", path);
        fclose(file);
        return;
    }
    buffer = (char *)malloc((size_t)file_size + 1);
    if (buffer == NULL) {
        fclose(file);
        return;
    }
    if (fread(buffer, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(buffer);
        fclose(file);
        return;
    }
    fclose(file);
    buffer[file_size] = '\0';
    root = cJSON_ParseWithLengthOpts(buffer, (size_t)file_size + 1, NULL, 1);
    free(buffer);
    if (root == NULL || !cJSON_IsObject(root)) {
        LOG_W(TAG, "设备 Linux 配置 %s 不是合法 JSON object，使用内置默认值", path);
        cJSON_Delete(root);
        return;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "enable");
    if (item != NULL && !cJSON_IsBool(item))
        LOG_W(TAG, "字段 enable 类型非法，忽略");
    else if (item != NULL)
        config->enable = cJSON_IsTrue(item) ? 1 : 0;

    item = cJSON_GetObjectItemCaseSensitive(root, "sta_interface");
    if (item != NULL && (!cJSON_IsString(item) || item->valuestring == NULL))
        LOG_W(TAG, "字段 sta_interface 类型非法，忽略");
    else if (item != NULL)
        snprintf(config->sta_interface, sizeof(config->sta_interface), "%s",
                 item->valuestring);

    item = cJSON_GetObjectItemCaseSensitive(root, "nvs_file");
    if (item != NULL && (!cJSON_IsString(item) || item->valuestring == NULL))
        LOG_W(TAG, "字段 nvs_file 类型非法，忽略");
    else if (item != NULL)
        snprintf(config->nvs_file, sizeof(config->nvs_file), "%s",
                 item->valuestring);

    cJSON_Delete(root);
}

/* ---------------- 错误映射 ---------------- */

static void set_backend_error(device_error_t *error, device_result_t code,
                              const char *operation, const char *message)
{
    if (error == NULL)
        return;
    memset(error, 0, sizeof(*error));
    error->code = code;
    error->platform_code = errno;
    if (operation != NULL)
        snprintf(error->operation, sizeof(error->operation), "%s", operation);
    if (message != NULL)
        snprintf(error->message, sizeof(error->message), "%s", message);
}

static device_result_t map_create_error(int result, const char *detail)
{
    char message[256];

    if (detail != NULL && detail[0] != '\0')
        snprintf(message, sizeof(message), "Linux 后端初始化失败: %s", detail);
    else
        snprintf(message, sizeof(message), "Linux 后端初始化失败");
    switch (result) {
    case DEMO_ERR_NOMEM:
        errno = ENOMEM;
        return DEVICE_ERR_NO_MEMORY;
    case DEMO_ERR_INVAL:
        errno = EINVAL;
        return DEVICE_ERR_INVALID_ARGUMENT;
    default:
        return DEVICE_ERR_BACKEND_INIT;
    }
}

/* ---------------- vtable（lb_ 前缀避免与公开 API 同名冲突） ---------------- */

static int lb_init(void *user, const char *config_path)
{
    (void)config_path;
    return user != NULL ? DEMO_OK : DEMO_ERR;
}

static void lb_deinit(void *user)
{
    (void)user;
}

static int lb_wifi_scan(void *user, net_ap_info_t *aps, int *count)
{
    return linux_wifi_scan(((linux_backend_t *)user)->wifi, aps, count);
}

static int lb_wifi_sta_connect(void *user, const char *ssid,
                               const char *password, wifi_reason_t *reason)
{
    return linux_wifi_connect(((linux_backend_t *)user)->wifi, ssid, password, reason);
}

static int lb_wifi_sta_disconnect(void *user)
{
    return linux_wifi_disconnect(((linux_backend_t *)user)->wifi);
}

static int lb_wifi_ap_start(void *user, const char *ssid, const char *password,
                            const char *pin)
{
    (void)user;
    (void)ssid;
    (void)password;
    (void)pin;
    /* beta v1.1：设备不再创建热点；纯 STA 实例对 AP 操作 fail-closed。 */
    return DEMO_ERR;
}

static int lb_wifi_ap_stop(void *user)
{
    (void)user;
    return DEMO_ERR;
}

static int lb_wifi_get_rssi(void *user, int *rssi)
{
    return linux_wifi_get_rssi(((linux_backend_t *)user)->wifi, rssi);
}

static int lb_wifi_get_ip(void *user, uint32_t *ip)
{
    return linux_wifi_get_ip(((linux_backend_t *)user)->wifi, ip);
}

static int lb_wifi_get_current_ssid(void *user, char *ssid, int capacity)
{
    return linux_wifi_get_current_ssid(((linux_backend_t *)user)->wifi, ssid, capacity);
}

static int lb_wifi_get_gateway(void *user, uint32_t *ip)
{
    return linux_wifi_get_gateway(((linux_backend_t *)user)->wifi, ip);
}

static int lb_tcp_listen(void *user, uint16_t port, void **sock)
{
    (void)user;
    return linux_socket_tcp_listen(port, sock);
}

static int lb_tcp_accept(void *user, void *listen, void **conn, net_addr_t *peer)
{
    (void)user;
    return linux_socket_tcp_accept(listen, conn, peer);
}

static int lb_tcp_connect(void *user, const net_addr_t *addr, void **sock,
                          int timeout_ms)
{
    (void)user;
    return linux_socket_tcp_connect(addr, sock, timeout_ms);
}

static int lb_sock_send(void *user, void *sock, const uint8_t *buf, int len)
{
    (void)user;
    return linux_socket_send(sock, buf, len);
}

static int lb_sock_recv(void *user, void *sock, uint8_t *buf, int cap)
{
    (void)user;
    return linux_socket_recv(sock, buf, cap);
}

static void lb_sock_close(void *user, void *sock)
{
    (void)user;
    linux_socket_close(sock);
}

static int lb_udp_mcast_join(void *user, const char *group, uint16_t port,
                             void **sock)
{
    (void)user;
    return linux_socket_udp_mcast_join(group, port, sock);
}

static int lb_udp_send(void *user, const char *group, uint16_t port,
                       const uint8_t *buf, int len)
{
    (void)user;
    return linux_socket_udp_send(group, port, buf, len);
}

static int lb_udp_recv(void *user, void *sock, uint8_t *buf, int cap,
                       net_addr_t *from)
{
    (void)user;
    return linux_socket_udp_recv(sock, buf, cap, from);
}

/*
 * mDNS 未在 Linux 后端实现（当前设备发现以 UDP 组播为主）。
 * 显式返回 DEMO_ERR（fail-closed，不回退任何降级路径）。
 */
static int lb_mdns_register(void *user, const net_mdns_service_t *svc)
{
    (void)user;
    (void)svc;
    return DEMO_ERR;
}

static int lb_mdns_unregister(void *user, const char *type)
{
    (void)user;
    (void)type;
    return DEMO_ERR;
}

static int lb_mdns_resolve(void *user, const char *type,
                           net_mdns_service_t *out, int timeout_ms)
{
    (void)user;
    (void)type;
    (void)out;
    (void)timeout_ms;
    return DEMO_ERR;
}

static int lb_nvs_get(void *user, const char *key, uint8_t *buf, int *len)
{
    return linux_nvs_get(((linux_backend_t *)user)->nvs, key, buf, len);
}

static int lb_nvs_set(void *user, const char *key, const uint8_t *buf, int len)
{
    return linux_nvs_set(((linux_backend_t *)user)->nvs, key, buf, len);
}

static int lb_nvs_erase(void *user, const char *key)
{
    return linux_nvs_erase(((linux_backend_t *)user)->nvs, key);
}

static uint64_t lb_time_ms(void *user)
{
    (void)user;
    return linux_socket_time_ms();
}

static uint32_t lb_random(void *user)
{
    (void)user;
    return linux_socket_random();
}

static int lb_inject(void *user, const char *action, const char *arg_json)
{
    (void)user;
    (void)action;
    (void)arg_json;
    return DEMO_ERR;
}

static const net_backend_t g_linux_backend_vtable = {
    lb_init, lb_deinit,
    lb_wifi_scan, lb_wifi_sta_connect, lb_wifi_sta_disconnect,
    lb_wifi_ap_start, lb_wifi_ap_stop,
    lb_wifi_get_rssi, lb_wifi_get_ip,
    lb_wifi_get_current_ssid, lb_wifi_get_gateway,
    lb_tcp_listen, lb_tcp_accept, lb_tcp_connect,
    lb_sock_send, lb_sock_recv, lb_sock_close,
    lb_udp_mcast_join, lb_udp_send, lb_udp_recv,
    lb_mdns_register, lb_mdns_unregister, lb_mdns_resolve,
    lb_nvs_get, lb_nvs_set, lb_nvs_erase,
    lb_time_ms, lb_random,
    lb_inject,
};

static void linux_backend_destroy_user(void *user)
{
    linux_backend_t *backend = (linux_backend_t *)user;

    if (backend == NULL)
        return;
    linux_wifi_destroy(backend->wifi);
    linux_nvs_destroy(backend->nvs);
    free(backend);
}

/* ---------------- 工厂入口 ---------------- */

device_result_t device_linux_backend_create(
    const device_linux_backend_options_t *options,
    device_backend_instance_t *out_instance,
    device_error_t *error)
{
    linux_backend_t *backend = NULL;
    linux_backend_config_t config;
    const char *nvs_file;
    const char *sta_interface;
    unsigned int device_index = 0;
    device_result_t result_code = DEVICE_OK;
    int result;

    if (out_instance != NULL)
        memset(out_instance, 0, sizeof(*out_instance));
    if (error != NULL)
        memset(error, 0, sizeof(*error));

    if (options != NULL && options->device_index > 15) {
        set_backend_error(error, DEVICE_ERR_INVALID_ARGUMENT,
                          "device_linux_backend_create",
                          "device_index 必须为 0 至 15");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    if (options != NULL)
        device_index = options->device_index;

    linux_backend_config_defaults(&config);
    if (options != NULL)
        linux_backend_config_load(&config, options->config_path);

    nvs_file = config.nvs_file;
    if (options != NULL && options->nvs_file != NULL && options->nvs_file[0] != '\0')
        nvs_file = options->nvs_file;
    sta_interface = config.sta_interface[0] != '\0' ? config.sta_interface : NULL;
    if (options != NULL && options->sta_interface != NULL &&
        options->sta_interface[0] != '\0')
        sta_interface = options->sta_interface;

    if (!config.enable) {
        LOG_E(TAG, "device_linux.json 禁用了真实 Linux 后端，拒绝启动");
        set_backend_error(error, DEVICE_ERR_BACKEND_UNAVAILABLE,
                          "device_linux_backend_create",
                          "device_linux.json 已禁用真实 Linux 后端");
        return DEVICE_ERR_BACKEND_UNAVAILABLE;
    }

    backend = (linux_backend_t *)calloc(1, sizeof(*backend));
    if (backend == NULL) {
        set_backend_error(error, DEVICE_ERR_NO_MEMORY,
                          "device_linux_backend_create", "内存不足");
        return DEVICE_ERR_NO_MEMORY;
    }

    /* beta v1.1：纯 STA 后端不创建需要 root 的 hotspot 实例，
     * 普通用户仅需 nmcli/网卡权限即可创建。 */
    result = linux_nvs_create(&backend->nvs, nvs_file);
    if (result != DEMO_OK) {
        result_code = map_create_error(result, NULL);
        set_backend_error(error, result_code, "device_linux_backend_create",
                          "文件 NVS 初始化失败");
        goto fail;
    }
    result = linux_wifi_create(&backend->wifi, sta_interface, NULL);
    if (result != DEMO_OK) {
        result_code = map_create_error(result, NULL);
        set_backend_error(error, result_code, "device_linux_backend_create",
                          "WiFi STA 初始化失败");
        goto fail;
    }

    (void)device_index; /* 当前驱动不需要按索引区分持久化文件名 */
    out_instance->vtable = &g_linux_backend_vtable;
    out_instance->user = backend;
    out_instance->destroy_user = linux_backend_destroy_user;
    if (error != NULL)
        error->code = DEVICE_OK;
    return DEVICE_OK;

fail:
    linux_backend_destroy_user(backend);
    return result_code;
}
