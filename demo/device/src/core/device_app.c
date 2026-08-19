#include "device_app.h"
#include "device_eventlog.h"
#include "device_limits.h"
#include "device_session.h"
#include "device_heal.h"
#include "log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <arpa/inet.h>
#endif

/* ---------------- 结构化事件上报（SS06 埋点） ---------------- */

void device_app_set_event_sink(device_app_t *app,
                               void (*fn)(void *user, const char *event,
                                          const char *result, int code,
                                          const char *data_json),
                               void *user)
{
    if (app == NULL)
        return;
    app->ev_fn = fn;
    app->ev_user = user;
}

void device_app_publish_event(device_app_t *app, const char *event,
                              const char *result, int code,
                              const char *data_json)
{
    if (app == NULL || app->ev_fn == NULL || event == NULL || result == NULL)
        return;
    app->ev_fn(app->ev_user, event, result, code, data_json);
}

/* ---------------- SHA-256（FIPS 180-4，纯 C，供事件 data.sha256） ---------------- */

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define SHA256_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_process_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = SHA256_ROR(w[i - 15], 7) ^ SHA256_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = SHA256_ROR(w[i - 2], 17) ^ SHA256_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = SHA256_ROR(e, 6) ^ SHA256_ROR(e, 11) ^ SHA256_ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = SHA256_ROR(a, 2) ^ SHA256_ROR(a, 13) ^ SHA256_ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void device_app_sha256_hex(const char *text, size_t len, char out_hex[65])
{
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint8_t block[64];
    size_t off = 0;

    while (off + 64 <= len) {
        sha256_process_block(h, (const uint8_t *)text + off);
        off += 64;
    }
    /* 剩余字节 + 填充 */
    size_t rem = len - off;
    memset(block, 0, sizeof(block));
    memcpy(block, text + off, rem);
    block[rem] = 0x80u;
    uint64_t bitlen = (uint64_t)len * 8;
    if (rem + 9 > 64) {
        sha256_process_block(h, block);
        memset(block, 0, sizeof(block));
    }
    for (int i = 0; i < 8; i++)
        block[56 + i] = (uint8_t)(bitlen >> (56 - i * 8));
    sha256_process_block(h, block);

    for (int i = 0; i < 8; i++) {
        snprintf(out_hex + i * 8, 9, "%08x", h[i]);
    }
    out_hex[64] = '\0';
}

#undef SHA256_ROR

/* ---------------- 内部工具 ---------------- */

void device_sleep_ms(int ms)
{
    if (ms < 0) ms = 0;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

uint32_t device_app_uptime_s(const device_app_t *app)
{
    return (uint32_t)((net_time_ms(app->net) - app->uptime_start_ms) / 1000);
}

/* IPv4 uint32 → "a.b.c.d"（仅用于日志/事件展示）。
 * 约定与 net_abstraction.h / sim_world / linux_wifi 一致：ip 内存字节序即点分
 * 四段顺序（等价 inet_addr 结果），因此低位字节是最左段。 */
static void ipv4_str(uint32_t ip, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

/* 安全 IPv4 解析（拒绝负号、越界、多余字段）；成功写入网络字节序 uint32
 * （与 sim_world_pc_ap_ip/linux_wifi_get_ip 同一字节序约定）。 */
static int parse_ipv4(const char *s, uint32_t *out)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = '\0';
    if (s == NULL || out == NULL)
        return DEMO_ERR_INVAL;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
        return DEMO_ERR_INVAL;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return DEMO_ERR_INVAL;
    *out = ((uint32_t)d << 24) | ((uint32_t)c << 16) | ((uint32_t)b << 8) | (uint32_t)a;
    return DEMO_OK;
}

int device_app_tx_text(device_app_t *app, const char *text)
{
    if (app == NULL || text == NULL || text[0] == '\0')
        return DEMO_ERR;
    if (strlen(text) > APP_DATA_TEXT_MAX)
        return DEMO_ERR;
    if (app->state != DEV_STATE_SESSION || app->sess_sock == NULL)
        return DEMO_ERR;
    if (!limits_allow_send(app, 0))
        return DEMO_ERR; /* 限速中：调用方保留待发，下轮重试 */
    cJSON *msg = cJSON_CreateObject();
    if (msg == NULL)
        return DEMO_ERR_NOMEM;
    cJSON_AddStringToObject(msg, "cmd", CMD_APP_DATA);
    cJSON_AddNumberToObject(msg, "seq", (int)++app->app_data_seq);
    cJSON *data = cJSON_AddObjectToObject(msg, "data");
    cJSON_AddStringToObject(data, "type", "debug_text");
    cJSON_AddStringToObject(data, "text", text);
    int rc = device_send_frame(app, app->sess_sock, msg);
    cJSON_Delete(msg);
    if (rc == DEMO_OK) {
        char data[128];
        char sha[65];
        device_app_sha256_hex(text, strlen(text), sha);
        snprintf(data, sizeof(data), "{\"text_bytes\":%d,\"sha256\":\"%s\"}",
                 (int)strlen(text), sha);
        device_app_publish_event(app, "app_data_tx", "ok", 0, data);
        LOG_I(app->device_id, "会话：已向上位机发送调试消息（%d 字节）",
              (int)strlen(text));
    } else
        LOG_W(app->device_id, "会话：联调消息发送失败（rc=%d），不再重发", rc);
    return rc;
}

void device_set_state(device_app_t *app, device_state_t s)
{
    if (app->state == s)
        return;
    LOG_I(app->device_id, "状态切换 -> %s", device_state_str(s));
    app->state = s;
    app->state_enter_ms = net_time_ms(app->net);
    app->snap_state = (int)s; /* 状态快照同步 */
    if (s == DEV_STATE_HEAL) {
        app->heal_enter_ms = app->state_enter_ms;
        app->heal_wait_ms = 0; /* 进入 HEAL 时重置退避等待，下一轮重算 */
    }
    if (s == DEV_STATE_SESSION)
        app->session_ack_ok = 0;
}

device_state_t device_app_get_state(const device_app_t *app) { return app->state; }

int device_send_frame_raw(device_app_t *app, void *sock, const char *json)
{
    uint8_t frame[PROTO_MSG_MAX_LEN + PROTO_FRAME_HEAD_LEN];
    int flen = frame_wrap((const uint8_t *)json, (int)strlen(json), frame, (int)sizeof(frame));
    if (flen < 0)
        return DEMO_ERR;
    int rc = net_sock_send(app->net, sock, frame, flen);
    if (rc >= 0) {
        LOG_I(app->device_id, "TX %s", json);
        return DEMO_OK;
    }
    return rc;
}

int device_send_frame(device_app_t *app, void *sock, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (s == NULL)
        return DEMO_ERR_NOMEM;
    int rc = device_send_frame_raw(app, sock, s);
    free(s);
    return rc;
}

/* ---------------- 收包统一入口（仅业务会话） ---------------- */

void device_app_handle_rx(device_app_t *app, void *sock)
{
    uint8_t buf[512];
    int n = net_sock_recv(app->net, sock, buf, (int)sizeof(buf));
    if (n == DEMO_ERR_AGAIN)
        return;
    if (n < 0) {
        /* 连接关闭或读取失败：通知会话层（由状态机处理）。 */
        LOG_W(app->device_id, "会话：接收失败（rc=%d）", n);
        app->rx_len = 0; /* 清残留帧缓冲 */
        app->rx_sock = sock;
        session_on_conn_closed(app);
        app->rx_sock = NULL;
        return;
    }

    app->rx_sock = sock;
    /* 累积到 rx_buf */
    if (app->rx_len + n > (int)sizeof(app->rx_buf)) {
        app->rx_len = 0; /* 溢出：丢弃缓冲（防呆） */
        app->malformed_count++;
        if (app->malformed_count >= app->params->malformed_max_per_conn) {
            LOG_W(app->device_id, "畸形报文过多，关闭连接");
            net_sock_close(app->net, sock);
            app->sess_sock = NULL;
            session_on_conn_closed(app);
            app->rx_sock = NULL;
            return;
        }
    }
    memcpy(app->rx_buf + app->rx_len, buf, (size_t)n);
    app->rx_len += n;

    /* 帧解析循环 */
    int consumed = 0;
    int off = 0, len = 0;
    while (app->rx_len > 0) {
        int rc = frame_parse(app->rx_buf, app->rx_len, &off, &len, &consumed);
        if (rc == 0)
            break; /* 需要更多数据 */
        if (rc == DEMO_ERR) {
            app->malformed_count++;
            LOG_W(app->device_id, "收到畸形帧（%d/%d）", app->malformed_count,
                  app->params->malformed_max_per_conn);
            if (app->malformed_count >= app->params->malformed_max_per_conn) {
                LOG_W(app->device_id, "畸形报文过多，关闭连接");
                net_sock_close(app->net, sock);
                app->rx_len = 0;
                app->sess_sock = NULL;
                session_on_conn_closed(app);
                app->rx_sock = NULL;
                return;
            }
            /* 丢弃畸形前缀，继续重同步 */
            memmove(app->rx_buf, app->rx_buf + consumed, (size_t)(app->rx_len - consumed));
            app->rx_len -= consumed;
            continue;
        }
        /* 完整帧 */
        app->rx_buf[off + len] = '\0';
        cJSON *json = cJSON_Parse((const char *)(app->rx_buf + off));
        if (json == NULL) {
            app->malformed_count++;
            LOG_W(app->device_id, "JSON 解析失败");
        } else {
            const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
            const char *cmdname = (cmd && cJSON_IsString(cmd)) ? cmd->valuestring : NULL;
            if (cmdname == NULL) {
                app->malformed_count++;
                LOG_W(app->device_id, "消息缺少 cmd 字段");
            } else {
                LOG_I(app->device_id, "RX %s", cmdname);
                session_on_msg(app, json);
            }
            cJSON_Delete(json);
        }
        /* 消费该帧 */
        memmove(app->rx_buf, app->rx_buf + consumed, (size_t)(app->rx_len - consumed));
        app->rx_len -= consumed;
    }
    app->rx_sock = NULL;
}

/* ---------------- NVS 读写（JSON blob，事件日志持久化用） ---------------- */

static int nvs_read_json(device_app_t *app, const char *key, cJSON **out)
{
    uint8_t buf[4096];
    int len = (int)sizeof(buf);
    *out = NULL;
    if (net_nvs_get(app->net, key, buf, &len) != DEMO_OK || len <= 0)
        return DEMO_ERR;
    buf[len] = '\0';
    cJSON *j = cJSON_Parse((const char *)buf);
    if (j == NULL)
        return DEMO_ERR;
    *out = j;
    return DEMO_OK;
}

/* ---------------- 生命周期 ---------------- */

device_app_t *device_app_create(const device_config_t *params, net_ctx_t *net,
                                const char *device_id, uint32_t seed)
{
    device_app_t *app = (device_app_t *)calloc(1, sizeof(device_app_t));
    if (app == NULL)
        return NULL;
    app->params = params;
    app->net = net;
    app->dev_index = (int)seed;
    snprintf(app->device_id, sizeof(app->device_id), "%s", device_id);
    app->state = DEV_STATE_BOOT;
    app->uptime_start_ms = net_time_ms(net);
    app->state_enter_ms = net_time_ms(net);
    app->next_tick_ms = 0;
    app->heartbeat_interval_ms = params->heartbeat_interval_ms;
    app->heartbeat_dead_ms = params->heartbeat_dead_ms > 0
        ? params->heartbeat_dead_ms
        : params->heartbeat_interval_ms * 3 / 2;

    /* 固定上位机地址：pc_host_ip:host_tcp_port → 网络字节序。
     * 配置校验（device_config_validate）已保证 ip/port 为契约值；此处仅做安全解析。 */
    app->sess_host.ip = 0;
    app->sess_host.port = dev_htons((uint16_t)params->host_tcp_port);
    if (parse_ipv4(params->pc_host_ip, &app->sess_host.ip) != DEMO_OK) {
        LOG_E(app->device_id, "固定上位机地址非法：%s", params->pc_host_ip);
        app->sess_host.ip = 0;
    }

    /* 从 NVS 恢复事件日志（环形缓冲持久化语义），再记录上电事件 */
    {
        cJSON *j = NULL;
        if (nvs_read_json(app, "evlog", &j) == DEMO_OK) {
            const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "events");
            if (cJSON_IsArray(arr)) {
                const cJSON *it;
                cJSON_ArrayForEach(it, arr) {
                    const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "m");
                    const cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "t");
                    if (!cJSON_IsString(m))
                        continue;
                    int idx = (app->evlog_head + app->evlog_count) % DEVICE_EVLOG_CAPACITY;
                    snprintf(app->evlog[idx].text, sizeof(app->evlog[idx].text), "%s",
                             m->valuestring);
                    app->evlog[idx].boot_s =
                        cJSON_IsNumber(t) ? (uint32_t)t->valueint : 0;
                    if (app->evlog_count < DEVICE_EVLOG_CAPACITY)
                        app->evlog_count++;
                    else
                        app->evlog_head = (app->evlog_head + 1) % DEVICE_EVLOG_CAPACITY;
                }
            }
            cJSON_Delete(j);
        }
    }
    evlog_record(app, "设备上电");
    return app;
}

void device_app_destroy(device_app_t *app)
{
    if (app == NULL)
        return;
    evlog_flush(app);
    if (app->sess_sock) {
        net_sock_close(app->net, app->sess_sock);
        app->sess_sock = NULL;
    }
    free(app);
}

void device_app_request_stop(device_app_t *app)
{
    if (app)
        dev_atomic_set(&app->stop_flag, 1);
}

/* ---------------- 状态机 ---------------- */

/* WiFi 有效：已连接且 IP 非 0 且实际 SSID 精确匹配目标。 */
static int wifi_connection_is_valid(device_app_t *app, char *actual_ssid,
                                    int actual_ssid_capacity)
{
    uint32_t ip = 0;
    char ssid[33] = {0};
    if (actual_ssid && actual_ssid_capacity > 0)
        actual_ssid[0] = '\0';
    if (net_wifi_get_ip(app->net, &ip) != DEMO_OK || ip == 0)
        return 0;
    if (net_wifi_get_current_ssid(app->net, ssid, (int)sizeof(ssid)) != DEMO_OK)
        return 0;
    if (strcmp(ssid, app->params->pc_ap_ssid) != 0)
        return 0;
    if (actual_ssid && actual_ssid_capacity > 0)
        snprintf(actual_ssid, (size_t)actual_ssid_capacity, "%s", ssid);
    return 1;
}

/* WiFi 扫描退避（指数，上限 cap，附加抖动）。 */
static long wifi_backoff_delay_ms(const device_app_t *app)
{
    int n = app->wifi_retry_count > 0 ? app->wifi_retry_count - 1 : 0;
    if (n > app->params->wifi_retry_max)
        n = app->params->wifi_retry_max;
    long delay = (long)app->params->wifi_backoff_base_ms;
    for (int i = 0; i < n; i++) {
        delay *= 2;
        if (delay > (long)app->params->wifi_backoff_cap_ms) {
            delay = (long)app->params->wifi_backoff_cap_ms;
            break;
        }
    }
    if (delay > (long)app->params->wifi_backoff_cap_ms)
        delay = (long)app->params->wifi_backoff_cap_ms;
    delay += (long)(net_random(app->net) %
                    (uint32_t)(app->params->wifi_backoff_jitter_ms + 1));
    return delay;
}

static void sm_boot(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    if (app->state_enter_ms == 0)
        app->state_enter_ms = now;
    /* 上电错峰 */
    if (now - app->state_enter_ms < (uint64_t)app->params->power_on_jitter_max_ms)
        return;
    app->wifi_retry_count = 0; /* 首次扫描立即执行 */
    device_set_state(app, DEV_STATE_WIFI_SCAN);
}

static void sm_wifi_scan(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    /* 退避门禁：失败后在退避期满前驻留，不重复扫描 */
    if (app->wifi_retry_count > 0) {
        long delay = wifi_backoff_delay_ms(app);
        if (now - app->state_enter_ms < (uint64_t)delay)
            return;
    }

    device_app_publish_event(app, "wifi_scan_started", "ok", 0, NULL);
    net_ap_info_t aps[DEVICE_SCAN_MAX_APS];
    int count = DEVICE_SCAN_MAX_APS;
    int found = -1;
    if (net_wifi_scan(app->net, aps, &count) == DEMO_OK) {
        for (int i = 0; i < count && i < DEVICE_SCAN_MAX_APS; i++) {
            if (aps[i].band_2g &&
                strcmp(aps[i].ssid, app->params->pc_ap_ssid) == 0) {
                found = i;
                break;
            }
        }
    }

    if (found >= 0) {
        char data[96];
        snprintf(data, sizeof(data), "{\"ssid\":\"%s\",\"rssi\":%d}",
                 app->params->pc_ap_ssid, aps[found].rssi);
        device_app_publish_event(app, "wifi_target_found", "ok", 0, data);
        evlog_record(app, "发现目标热点：%s", app->params->pc_ap_ssid);
        app->wifi_retry_count = 0;
        device_set_state(app, DEV_STATE_STA_JOIN);
        return;
    }

    evlog_record(app, "未发现目标热点，继续扫描（第 %d 次）", app->wifi_retry_count + 1);
    app->wifi_retry_count++;
    app->state_enter_ms = now; /* 退避窗口起点 */
}

static void sm_sta_join(device_app_t *app)
{
    wifi_reason_t reason = WIFI_REASON_OK;
    int rc = net_wifi_sta_connect(app->net, app->params->pc_ap_ssid,
                                  app->params->pc_ap_password, &reason);
    if (rc != DEMO_OK) {
        char data[64];
        snprintf(data, sizeof(data), "{\"reason\":%d}", (int)reason);
        device_app_publish_event(app, "wifi_connect_failed", "fail", (int)reason, data);
        evlog_record(app, "WiFi 连接失败（reason=%d）：%s", (int)reason,
                     app->params->pc_ap_ssid);
        if (reason == WIFI_REASON_AUTH_FAIL)
            app->err_auth_fails++;
        else
            app->err_wifi_disconnects++;
        app->wifi_retry_count++;
        device_set_state(app, DEV_STATE_HEAL);
        return;
    }

    uint32_t ip = 0;
    char actual_ssid[33] = {0};
    if (!wifi_connection_is_valid(app, actual_ssid, (int)sizeof(actual_ssid))) {
        device_app_publish_event(app, "wifi_connect_failed", "fail",
                                 WIFI_REASON_HANDSHAKE_TIMEOUT,
                                 "{\"reason\":205}");
        evlog_record(app, "WiFi 连接校验失败（IP/SSID）：%s", app->params->pc_ap_ssid);
        net_wifi_sta_disconnect(app->net);
        app->err_wifi_disconnects++;
        app->wifi_retry_count++;
        device_set_state(app, DEV_STATE_HEAL);
        return;
    }
    net_wifi_get_ip(app->net, &ip);
    {
        char ipbuf[16];
        char data[96];
        ipv4_str(ip, ipbuf, sizeof(ipbuf));
        snprintf(data, sizeof(data), "{\"ssid\":\"%s\",\"device_ip\":\"%s\"}",
                 actual_ssid, ipbuf);
        device_app_publish_event(app, "wifi_connected", "ok", 0, data);
    }
    evlog_record(app, "WiFi 已连接：%s", actual_ssid);
    app->wifi_retry_count = 0;
    device_set_state(app, DEV_STATE_CONNECT);
}

static void sm_connect(device_app_t *app)
{
    {
        char ipbuf[16];
        char data[96];
        ipv4_str(app->sess_host.ip, ipbuf, sizeof(ipbuf));
        snprintf(data, sizeof(data), "{\"host\":\"%s\",\"port\":%d}",
                 ipbuf, (int)dev_ntohs(app->sess_host.port));
        device_app_publish_event(app, "tcp_connecting", "ok", 0, data);
    }
    int rc = session_connect(app, &app->sess_host);
    if (rc == DEMO_OK) {
        device_set_state(app, DEV_STATE_SESSION);
        return;
    }
    /* 连接失败：进入 HEAL 退避重连 */
    evlog_record(app, "连接上位机失败，进入自愈阶段");
    heal_on_disconnect(app, "连接失败");
    device_set_state(app, DEV_STATE_HEAL);
}

static void sm_session(device_app_t *app)
{
    /* WiFi 失效直接断开并进入 HEAL（HEAL 会判定回扫描） */
    if (!wifi_connection_is_valid(app, NULL, 0)) {
        evlog_record(app, "会话期间 WiFi 失效，进入自愈阶段");
        device_app_publish_event(app, "session_offline", "ok", 0,
                                 "{\"reason\":\"wifi_lost\"}");
        session_disconnect(app);
        heal_on_disconnect(app, "WiFi 失效");
        device_set_state(app, DEV_STATE_HEAL);
        return;
    }
    int alive = session_poll(app);
    if (!alive) {
        evlog_record(app, "会话丢失，进入自愈阶段");
        device_app_publish_event(app, "session_offline", "ok", 0,
                                 "{\"reason\":\"disconnected\"}");
        heal_on_disconnect(app, "会话丢失");
        device_set_state(app, DEV_STATE_HEAL);
        return;
    }
    /* RSSI 监测（自愈） */
    if (heal_rssi_poll(app)) {
        evlog_record(app, "RSSI 持续偏低，主动重新连接");
        session_disconnect(app);
        heal_on_disconnect(app, "RSSI 持续偏低");
        device_set_state(app, DEV_STATE_HEAL);
    }
}

static void sm_heal(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    if (!wifi_connection_is_valid(app, NULL, 0)) {
        /* WiFi 失效：断开 STA，回扫描（按 WiFi 退避） */
        evlog_record(app, "WiFi 失效，断开并返回扫描");
        net_wifi_sta_disconnect(app->net);
        app->wifi_retry_count++;
        device_set_state(app, DEV_STATE_WIFI_SCAN);
        return;
    }

    /* WiFi 健康：按 TCP 退避重连固定地址 */
    if (app->heal_wait_ms <= 0)
        app->heal_wait_ms = heal_next_backoff_ms(app);
    if (now - app->state_enter_ms >= (uint64_t)app->heal_wait_ms) {
        int rc = session_connect(app, &app->sess_host);
        if (rc == DEMO_OK) {
            evlog_record(app, "已重新连接上位机");
            heal_reset_backoff(app);
            device_set_state(app, DEV_STATE_SESSION);
            return;
        }
        /* 重连失败：重新计时退避 */
        app->state_enter_ms = now;
        app->heal_wait_ms = 0;
    }
}

/* ---------------- 事件循环 ---------------- */

int device_app_run_step(device_app_t *app)
{
    if (app == NULL)
        return 0;
    uint64_t now = net_time_ms(app->net);
    /* 状态快照节流更新（500ms）：rssi / uptime */
    if (now - app->last_snap_ms >= 500) {
        app->last_snap_ms = now;
        int rssi = 0;
        net_wifi_get_rssi(app->net, &rssi);
        app->snap_rssi = rssi;
        app->snap_uptime_s = (uint32_t)((now - app->uptime_start_ms) / 1000);
    }
    if (now >= app->next_tick_ms) {
        switch (app->state) {
        case DEV_STATE_BOOT:       sm_boot(app); break;
        case DEV_STATE_WIFI_SCAN:  sm_wifi_scan(app); break;
        case DEV_STATE_STA_JOIN:   sm_sta_join(app); break;
        case DEV_STATE_CONNECT:    sm_connect(app); break;
        case DEV_STATE_SESSION:    sm_session(app); break;
        case DEV_STATE_HEAL:       sm_heal(app); break;
        default: break;
        }
        app->next_tick_ms = net_time_ms(app->net) + 10;
    }
    device_sleep_ms(10);
    return !dev_atomic_get(&app->stop_flag);
}

void device_app_run(device_app_t *app)
{
    if (app == NULL)
        return;
    app->next_tick_ms = net_time_ms(app->net) + 10;
    while (device_app_run_step(app)) {
    }
    LOG_I(app->device_id, "设备运行循环已停止");
}
