#include "linux_wifi.h"

#include "linux_exec.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "LINUX_WIFI"

struct linux_wifi {
    char sta_iface[32];
    linux_wifi_ops_t ops;   /* 复制注入表，保留 exec_argv；无注入时置 NULL */
};

static int exec_capture(linux_wifi_t *wifi, const char *const argv[],
                        char *output, size_t output_capacity)
{
    if (wifi->ops.exec_argv != NULL)
        return wifi->ops.exec_argv(argv, output, output_capacity);
    return linux_exec_argv(argv, output, output_capacity);
}

int linux_wifi_create(linux_wifi_t **out, const char *sta_interface,
                      const linux_wifi_ops_t *ops_override)
{
    linux_wifi_t *wifi;

    if (out == NULL)
        return DEMO_ERR_INVAL;
    *out = NULL;
    wifi = (linux_wifi_t *)calloc(1, sizeof(*wifi));
    if (wifi == NULL)
        return DEMO_ERR_NOMEM;
    if (ops_override != NULL)
        wifi->ops = *ops_override;
    if (sta_interface != NULL && sta_interface[0] != '\0')
        snprintf(wifi->sta_iface, sizeof(wifi->sta_iface), "%s", sta_interface);
    else
        snprintf(wifi->sta_iface, sizeof(wifi->sta_iface), "%s", "wlP2p33s0");
    LOG_I(TAG, "WiFi STA 接口: %s", wifi->sta_iface);
    *out = wifi;
    return DEMO_OK;
}

void linux_wifi_destroy(linux_wifi_t *wifi)
{
    if (wifi == NULL)
        return;
    memset(wifi, 0, sizeof(*wifi));
    free(wifi);
}

const char *linux_wifi_sta_interface(const linux_wifi_t *wifi)
{
    if (wifi == NULL || wifi->sta_iface[0] == '\0')
        return "wlP2p33s0";
    return wifi->sta_iface;
}

static int wait_for_ip(linux_wifi_t *wifi, int timeout_sec)
{
    int i;

    for (i = 0; i < timeout_sec; ++i) {
        uint32_t ip = 0;
        if (linux_wifi_get_ip(wifi, &ip) == DEMO_OK && ip != 0)
            return DEMO_OK;
        sleep(1);
    }
    return DEMO_ERR;
}

/* ---------------- 公开接口 ---------------- */

int linux_wifi_connect(linux_wifi_t *wifi, const char *ssid,
                       const char *password, wifi_reason_t *reason)
{
    char outbuf[2048];
    const char *const argv[] = {
        "nmcli", "dev", "wifi", "connect", ssid, "password", password,
        "ifname", wifi->sta_iface, NULL
    };

    if (wifi == NULL) {
        if (reason != NULL)
            *reason = WIFI_REASON_NO_AP_FOUND;
        return DEMO_ERR;
    }
    if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
        if (reason != NULL)
            *reason = WIFI_REASON_NO_AP_FOUND;
        return DEMO_ERR;
    }
    LOG_I(TAG, "尝试连接 WiFi: SSID=%s, iface=%s", ssid, wifi->sta_iface);

    outbuf[0] = '\0';
    exec_capture(wifi, argv, outbuf, sizeof(outbuf));

    LOG_D(TAG, "nmcli 输出: %s", outbuf);

    if (strstr(outbuf, "successfully activated") || strstr(outbuf, "已激活")) {
        LOG_I(TAG, "nmcli 连接成功，等待 DHCP...");
        if (wait_for_ip(wifi, 10) == DEMO_OK && linux_wifi_is_connected_to(wifi, ssid)) {
            uint32_t ip = 0;
            linux_wifi_get_ip(wifi, &ip);
            LOG_I(TAG, "WiFi 已连接，IP=%u.%u.%u.%u",
                  ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
            return DEMO_OK;
        }
        if (!linux_wifi_is_connected_to(wifi, ssid)) {
            LOG_W(TAG, "已连接但 SSID 不匹配: 期望 %s，实际连接可能已变更", ssid);
        }
        LOG_W(TAG, "nmcli 连接成功但 DHCP 超时");
        if (reason != NULL)
            *reason = WIFI_REASON_HANDSHAKE_TIMEOUT;
        return DEMO_ERR;
    }
    if (strstr(outbuf, "Secrets were required") || strstr(outbuf, "密码") ||
        strstr(outbuf, "passphrase") || strstr(outbuf, "802.1X") ||
        strstr(outbuf, "Error: 802.1X")) {
        LOG_W(TAG, "WiFi 认证失败（密码错误）: %s", ssid);
        if (reason != NULL)
            *reason = WIFI_REASON_AUTH_FAIL;
        return DEMO_ERR;
    }
    if (strstr(outbuf, "No network with SSID") || strstr(outbuf, "找不到") ||
        strstr(outbuf, "not found")) {
        LOG_W(TAG, "WiFi SSID 未找到: %s", ssid);
        if (reason != NULL)
            *reason = WIFI_REASON_NO_AP_FOUND;
        return DEMO_ERR;
    }
    if (linux_wifi_is_connected_to(wifi, ssid)) {
        LOG_I(TAG, "nmcli 返回异常但 SSID 已匹配");
        return DEMO_OK;
    }
    LOG_W(TAG, "WiFi 连接失败: %s", ssid);
    if (reason != NULL)
        *reason = WIFI_REASON_NO_AP_FOUND;
    return DEMO_ERR;
}

int linux_wifi_disconnect(linux_wifi_t *wifi)
{
    const char *const argv[] = {"nmcli", "dev", "disconnect", wifi->sta_iface, NULL};

    if (wifi == NULL)
        return DEMO_ERR;
    LOG_I(TAG, "断开 WiFi: %s", wifi->sta_iface);
    exec_capture(wifi, argv, NULL, 0);
    return DEMO_OK;
}

int linux_wifi_get_ip(linux_wifi_t *wifi, uint32_t *ip)
{
    char outbuf[256];
    unsigned a = 0, b = 0, c = 0, d = 0;
    const char *const argv[] = {"ip", "-4", "addr", "show", "dev",
                                wifi->sta_iface, NULL};

    if (wifi == NULL || ip == NULL)
        return DEMO_ERR;
    *ip = 0;
    outbuf[0] = '\0';
    if (exec_capture(wifi, argv, outbuf, sizeof(outbuf)) != 0 || outbuf[0] == '\0')
        return DEMO_ERR;
    {
        const char *position = strstr(outbuf, "inet ");
        if (position != NULL && sscanf(position + 5, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            *ip = (d << 24) | (c << 16) | (b << 8) | a;
            return DEMO_OK;
        }
    }
    return DEMO_ERR;
}

int linux_wifi_get_rssi(linux_wifi_t *wifi, int *rssi)
{
    char outbuf[256];
    const char *const iw_argv[] = {"iw", "dev", wifi->sta_iface, "link", NULL};
    const char *const nmcli_argv[] = {"nmcli", "-t", "-f", "IN-USE,SIGNAL",
                                      "dev", "wifi", "list", NULL};

    if (wifi == NULL || rssi == NULL)
        return DEMO_ERR;
    *rssi = -100;
    outbuf[0] = '\0';
    if (exec_capture(wifi, iw_argv, outbuf, sizeof(outbuf)) == 0 &&
        outbuf[0] != '\0') {
        const char *position = strstr(outbuf, "signal: ");
        if (position != NULL && sscanf(position + 8, "%d", rssi) == 1)
            return DEMO_OK;
    }
    outbuf[0] = '\0';
    if (exec_capture(wifi, nmcli_argv, outbuf, sizeof(outbuf)) == 0 &&
        outbuf[0] != '\0') {
        char *line = outbuf;
        while (*line != '\0') {
            char *eol = strchr(line, '\n');
            int signal = 0;
            if (eol != NULL)
                *eol = '\0';
            if (line[0] == '*' && sscanf(line, "*:%d", &signal) == 1 && signal > 0) {
                *rssi = -signal;
                return DEMO_OK;
            }
            if (eol == NULL)
                break;
            line = eol + 1;
        }
    }
    return DEMO_ERR;
}

int linux_wifi_is_connected(linux_wifi_t *wifi)
{
    uint32_t ip = 0;
    return (linux_wifi_get_ip(wifi, &ip) == DEMO_OK && ip != 0) ? 1 : 0;
}

static int get_current_ssid(linux_wifi_t *wifi, char *ssid_out, size_t cap)
{
    char outbuf[256];
    const char *const argv[] = {"iw", "dev", wifi->sta_iface, "info", NULL};

    if (wifi == NULL || ssid_out == NULL || cap == 0)
        return DEMO_ERR;
    ssid_out[0] = '\0';
    outbuf[0] = '\0';
    if (exec_capture(wifi, argv, outbuf, sizeof(outbuf)) != 0 || outbuf[0] == '\0')
        return DEMO_ERR;
    {
        const char *position = strstr(outbuf, "ssid ");
        if (position == NULL)
            return DEMO_ERR;
        position += 5;
        {
            const char *end = strchr(position, '\n');
            size_t length = end == NULL ? strlen(position)
                                        : (size_t)(end - position);
            if (length >= cap)
                length = cap - 1;
            memcpy(ssid_out, position, length);
            ssid_out[length] = '\0';
            return DEMO_OK;
        }
    }
}

int linux_wifi_is_connected_to(linux_wifi_t *wifi, const char *ssid)
{
    char current[64];

    if (ssid == NULL || wifi == NULL)
        return 0;
    if (get_current_ssid(wifi, current, sizeof(current)) != DEMO_OK)
        return 0;
    return strcmp(current, ssid) == 0 ? 1 : 0;
}

int linux_wifi_get_current_ssid(linux_wifi_t *wifi, char *ssid, int capacity)
{
    if (ssid == NULL || capacity <= 0)
        return DEMO_ERR_INVAL;
    return get_current_ssid(wifi, ssid, (size_t)capacity);
}

int linux_wifi_get_gateway(linux_wifi_t *wifi, uint32_t *ip)
{
    char outbuf[256];
    const char *const argv[] = {"ip", "route", "show", "dev", wifi->sta_iface,
                                NULL};

    if (wifi == NULL || ip == NULL)
        return DEMO_ERR;
    *ip = 0;
    outbuf[0] = '\0';
    if (exec_capture(wifi, argv, outbuf, sizeof(outbuf)) != 0 || outbuf[0] == '\0')
        return DEMO_ERR;
    {
        const char *position = strstr(outbuf, "via ");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (position == NULL)
            return DEMO_ERR;
        if (sscanf(position + 4, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            *ip = (d << 24) | (c << 16) | (b << 8) | a;
            return DEMO_OK;
        }
    }
    return DEMO_ERR;
}

int linux_wifi_scan(linux_wifi_t *wifi, net_ap_info_t *aps, int *count)
{
    char outbuf[8192];
    char *line;
    int n;
    int cap;
    const char *const argv[] = {"nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY",
                                "dev", "wifi", "list", "ifname",
                                wifi->sta_iface, NULL};

    if (wifi == NULL || aps == NULL || count == NULL || *count <= 0)
        return DEMO_ERR;
    LOG_D(TAG, "扫描 WiFi 网络...");
    outbuf[0] = '\0';
    if (exec_capture(wifi, argv, outbuf, sizeof(outbuf)) != 0 || outbuf[0] == '\0') {
        LOG_W(TAG, "nmcli 扫描命令执行失败或无输出（iface=%s）", wifi->sta_iface);
        return DEMO_ERR;
    }

    n = 0;
    cap = *count;
    line = outbuf;
    while (n < cap) {
        char *eol = strchr(line, '\n');
        char ssid[64];
        int signal = 0;

        if (eol != NULL)
            *eol = '\0';
        if (line[0] == '\0')
            break;
        ssid[0] = '\0';
        if (sscanf(line, "%63[^:]:%d:", ssid, &signal) >= 1 && ssid[0] != '\0') {
            memset(&aps[n], 0, sizeof(aps[n]));
            snprintf(aps[n].ssid, sizeof(aps[n].ssid), "%s", ssid);
            aps[n].rssi = signal > 0 ? -signal : signal;
            aps[n].band_2g = 1;
            ++n;
        }
        if (eol == NULL)
            break;
        line = eol + 1;
        if (*line == '\0')
            break;
    }
    *count = n;
    LOG_D(TAG, "扫描完成: %d 个网络", n);
    return DEMO_OK;
}
