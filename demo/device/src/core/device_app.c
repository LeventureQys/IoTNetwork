#include "device_app.h"
#include "device_eventlog.h"
#include "device_limits.h"
#include "device_provision.h"
#include "device_discovery.h"
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
    if (s == DEV_STATE_HEAL)
        app->heal_enter_ms = app->state_enter_ms;
    if (s == DEV_STATE_SESSION)
    {
        app->cred_confirm_done = 0;
        app->session_ack_ok = 0;
    }
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

/* ---------------- 收包统一入口 ---------------- */

void device_app_handle_rx(device_app_t *app, void *sock, int is_business)
{
    uint8_t buf[512];
    int n = net_sock_recv(app->net, sock, buf, (int)sizeof(buf));
    if (n == DEMO_ERR_AGAIN)
        return;
    if (n < 0) {
        /* 连接关闭：通知会话层（由状态机处理） */
        app->rx_len = 0; /* 清残留帧缓冲 */
        app->rx_sock = sock;
        if (is_business)
            session_on_conn_closed(app);
        else
            prov_server_on_conn_closed(app);
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
            if (is_business) {
                app->sess_sock = NULL;
                session_on_conn_closed(app);
            } else {
                prov_server_on_conn_closed(app);
            }
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
                if (is_business) {
                    app->sess_sock = NULL;
                    session_on_conn_closed(app);
                } else {
                    prov_server_on_conn_closed(app);
                }
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
                if (is_business) {
                    session_on_msg(app, json);
                } else {
                    prov_server_on_msg(app, json);
                }
            }
            cJSON_Delete(json);
        }
        /* 消费该帧 */
        memmove(app->rx_buf, app->rx_buf + consumed, (size_t)(app->rx_len - consumed));
        app->rx_len -= consumed;
    }
    app->rx_sock = NULL;
}

/* ---------------- NVS 读写（JSON blob） ---------------- */

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

static int nvs_write_json(device_app_t *app, const char *key, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (s == NULL)
        return DEMO_ERR_NOMEM;
    int rc = net_nvs_set(app->net, key, (const uint8_t *)s, (int)strlen(s));
    free(s);
    return rc;
}

static int creds_save(device_app_t *app)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return DEMO_ERR_NOMEM;
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON *arr = cJSON_AddArrayToObject(root, "creds");
    for (int i = 0; i < app->cred_count; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ssid", app->creds[i]);
        cJSON_AddStringToObject(it, "password", app->cred_pass[i]);
        cJSON_AddNumberToObject(it, "confirmed", app->cred_confirmed[i]);
        cJSON_AddItemToArray(arr, it);
    }
    int rc = nvs_write_json(app, "wifi_creds", root);
    cJSON_Delete(root);
    return rc;
}

static void creds_remove(device_app_t *app, int index)
{
    if (index < 0 || index >= app->cred_count)
        return;
    for (int i = index; i < app->cred_count - 1; i++) {
        snprintf(app->creds[i], sizeof(app->creds[i]), "%s", app->creds[i + 1]);
        snprintf(app->cred_pass[i], sizeof(app->cred_pass[i]), "%s", app->cred_pass[i + 1]);
        app->cred_confirmed[i] = app->cred_confirmed[i + 1];
    }
    app->cred_count--;
    app->creds[app->cred_count][0] = '\0';
    app->cred_pass[app->cred_count][0] = '\0';
    app->cred_confirmed[app->cred_count] = 0;
    if (app->cred_active >= app->cred_count)
        app->cred_active = 0;
}

/* 凭据持久化：{ "schema":1, "creds":[ {ssid,password,confirmed}, ... ] } */
void device_creds_reload(device_app_t *app)
{
    app->cred_count = 0;
    app->cred_active = 0;
    cJSON *j = NULL;
    if (nvs_read_json(app, "wifi_creds", &j) != DEMO_OK)
        return;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "creds");
    if (cJSON_IsArray(arr)) {
        int n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (n >= PROTO_WIFI_CRED_MAX)
                break;
            const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(it, "ssid");
            const cJSON *pass = cJSON_GetObjectItemCaseSensitive(it, "password");
            const cJSON *cfm = cJSON_GetObjectItemCaseSensitive(it, "confirmed");
            if (!cJSON_IsString(ssid) || !cJSON_IsString(pass))
                continue;
            snprintf(app->creds[n], sizeof(app->creds[n]), "%s", ssid->valuestring);
            snprintf(app->cred_pass[n], sizeof(app->cred_pass[n]), "%s", pass->valuestring);
            app->cred_confirmed[n] = (cJSON_IsNumber(cfm) && cfm->valueint != 0) ? 1 : 0;
            n++;
        }
        app->cred_count = n;
    }
    cJSON_Delete(j);

    /* 未确认即重启必须回滚；没有 last-known-good 时清除并重新配网。 */
    int rolled_back = 0;
    for (int i = 0; i < app->cred_count; i++) {
        if (!app->cred_confirmed[i]) {
            evlog_record(app, "未确认的网络凭据已回滚");
            LOG_W(app->device_id, "网络凭据已回滚：%s", app->creds[i]);
            creds_remove(app, i);
            rolled_back = 1;
            i--;
        }
    }
    if (rolled_back) {
        if (app->cred_count > 0)
            creds_save(app);
        else
            net_nvs_erase(app->net, "wifi_creds");
    }
}

static void creds_clear_all(device_app_t *app)
{
    net_nvs_erase(app->net, "wifi_creds");
    app->cred_count = 0;
    app->cred_active = 0;
    for (int i = 0; i < PROTO_WIFI_CRED_MAX; i++) {
        app->creds[i][0] = '\0';
        app->cred_pass[i][0] = '\0';
        app->cred_confirmed[i] = 0;
    }
    evlog_record(app, "所有网络凭据已清除");
}

static int wifi_matches_credential(device_app_t *app, int index, char *actual_ssid,
                                   int actual_ssid_capacity)
{
    char current_ssid[33] = {0};
    if (index < 0 || index >= app->cred_count ||
        net_wifi_get_current_ssid(app->net, current_ssid, (int)sizeof(current_ssid)) != DEMO_OK)
        return 0;
    if (actual_ssid && actual_ssid_capacity > 0)
        snprintf(actual_ssid, (size_t)actual_ssid_capacity, "%s", current_ssid);
    if (strcmp(current_ssid, app->creds[index]) != 0) {
        LOG_W(app->device_id, "WiFi SSID 不匹配：期望=%s，实际=%s",
              app->creds[index], current_ssid);
        return 0;
    }
    return 1;
}

static int wifi_connection_is_valid(device_app_t *app, char *actual_ssid,
                                    int actual_ssid_capacity)
{
    uint32_t ip = 0;
    if (actual_ssid && actual_ssid_capacity > 0)
        actual_ssid[0] = '\0';
    return net_wifi_get_ip(app->net, &ip) == DEMO_OK && ip != 0 &&
           wifi_matches_credential(app, app->cred_active, actual_ssid,
                                   actual_ssid_capacity);
}

static int ensure_expected_wifi_or_recover(device_app_t *app, const char *stage,
                                           int reprovision_if_disconnected)
{
    char actual_ssid[33] = {0};
    if (wifi_connection_is_valid(app, actual_ssid, (int)sizeof(actual_ssid)))
        return 1;

    const char *expected = (app->cred_active >= 0 && app->cred_active < app->cred_count)
        ? app->creds[app->cred_active] : "<无>";
    if (actual_ssid[0] == '\0' && !reprovision_if_disconnected) {
        LOG_W(app->device_id, "%s：目标 WiFi 已断开，返回 STA 重连流程（期望=%s）",
              stage, expected);
        evlog_record(app, "%s：目标 WiFi 已断开，返回 STA 重连", stage);
        session_disconnect(app);
        discovery_stop(app);
        app->wifi_retry_count = 0;
        device_set_state(app, DEV_STATE_STA_JOIN);
        return 0;
    }
    LOG_W(app->device_id,
          "%s：目标 WiFi 校验失败，期望=%s，实际=%s；清除凭据并重新开启配网热点",
          stage, expected, actual_ssid[0] ? actual_ssid : "<未连接>");
    evlog_record(app, "%s：目标 WiFi 校验失败，清除凭据并重新配网", stage);
    session_disconnect(app);
    discovery_stop(app);
    net_wifi_sta_disconnect(app->net);
    creds_clear_all(app);
    app->provision_confirm_deadline_ms = 0;
    app->provision_auth_ok = 0;
    app->provision_wifi_ok = 0;
    device_set_state(app, DEV_STATE_AP_PROVISION);
    return 0;
}

static void rollback_unconfirmed_credential(device_app_t *app, const char *reason)
{
    int index = app->cred_active;
    if (index < 0 || index >= app->cred_count || app->cred_confirmed[index])
        return;
    evlog_record(app, "未确认网络已移除：%s（%s）", app->creds[index], reason);
    session_disconnect(app);
    net_wifi_sta_disconnect(app->net);
    creds_remove(app, index);
    app->provision_confirm_deadline_ms = 0;
    if (app->cred_count > 0) {
        creds_save(app);
        app->cred_active = 0;
        app->wifi_retry_count = 0;
        device_set_state(app, DEV_STATE_STA_JOIN);
    } else {
        net_nvs_erase(app->net, "wifi_creds");
        device_set_state(app, DEV_STATE_AP_PROVISION);
    }
}

static void candidates_load(device_app_t *app)
{
    app->candidate_count = 0;
    cJSON *j = NULL;
    if (nvs_read_json(app, "host_candidates", &j) != DEMO_OK)
        return;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "candidates");
    if (cJSON_IsArray(arr)) {
        int n = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (n >= PROTO_CANDIDATE_MAX)
                break;
            const cJSON *ip = cJSON_GetObjectItemCaseSensitive(it, "ip");
            const cJSON *port = cJSON_GetObjectItemCaseSensitive(it, "port");
            if (!cJSON_IsString(ip) || !cJSON_IsNumber(port))
                continue;
            unsigned a, b, c, d;
            if (sscanf(ip->valuestring, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
                continue;
            app->candidate_list[n].ip = (uint32_t)((d << 24) | (c << 16) | (b << 8) | a);
            app->candidate_list[n].port = htons((uint16_t)port->valueint);
            n++;
        }
        app->candidate_count = n;
    }
    cJSON_Delete(j);
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
    /* 热点名 = Modu_ + MAC 最后两个十六进制字节（去冒号，大写） */
    {
        size_t len = strlen(device_id);
        char tail[5];
        tail[0] = (char)toupper((unsigned char)device_id[len - 5]);
        tail[1] = (char)toupper((unsigned char)device_id[len - 4]);
        tail[2] = (char)toupper((unsigned char)device_id[len - 2]);
        tail[3] = (char)toupper((unsigned char)device_id[len - 1]);
        tail[4] = '\0';
        snprintf(app->ap_ssid, sizeof(app->ap_ssid), "Modu_%s", tail);
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
    device_creds_reload(app);
    candidates_load(app);
    return app;
}

void device_app_destroy(device_app_t *app)
{
    if (app == NULL)
        return;
    evlog_flush(app);
    if (app->mcast_sock) {
        net_sock_close(app->net, app->mcast_sock);
        app->mcast_sock = NULL;
    }
    if (app->sess_sock) {
        net_sock_close(app->net, app->sess_sock);
        app->sess_sock = NULL;
    }
    prov_server_stop(app);
    free(app);
}

void device_app_request_stop(device_app_t *app)
{
    if (app)
        dev_atomic_set(&app->stop_flag, 1);
}

/* ---------------- 状态机 ---------------- */

static void sm_boot(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    if (app->state_enter_ms == 0)
        app->state_enter_ms = now;
    /* 热点/配网失败退避（provision_ap_backoff） */
    if (app->boot_backoff_until > now)
        return;
    /* 上电错峰 */
    if (now - app->state_enter_ms < (uint64_t)app->params->power_on_jitter_max_ms)
        return;
    if (app->cred_count > 0) {
        app->cred_active = 0;
        device_set_state(app, DEV_STATE_STA_JOIN);
    } else {
        device_set_state(app, DEV_STATE_AP_PROVISION);
    }
}

static void sm_sta_join(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    /* 退避门禁：上次尝试失败后，在退避期满前驻留，不发起新的连接尝试 */
    if (app->wifi_retry_count > 0) {
        int base = app->params->wifi_backoff_base_ms;
        int cap = app->params->wifi_backoff_cap_ms;
        long delay = (long)base << (app->wifi_retry_count - 1);
        if (delay > cap) delay = cap;
        delay += (long)(net_random(app->net) % (uint32_t)(app->params->wifi_backoff_jitter_ms + 1));
        if (now - app->state_enter_ms < (uint64_t)delay)
            return;
    }
    wifi_reason_t reason = WIFI_REASON_OK;
    int rc = net_wifi_sta_connect(app->net, app->creds[app->cred_active],
                                  app->cred_pass[app->cred_active], &reason);
    if (rc == DEMO_OK) {
        uint32_t ip = 0;
        net_wifi_get_ip(app->net, &ip);
        if (ip == 0 || !wifi_matches_credential(app, app->cred_active, NULL, 0)) {
            evlog_record(app, "WiFi 连接校验失败：%s", app->creds[app->cred_active]);
            net_wifi_sta_disconnect(app->net);
            app->err_wifi_disconnects++;
            app->cred_active++;
            app->wifi_retry_count = 0;
            if (app->cred_active >= app->cred_count) {
                evlog_record(app, "所有网络凭据均不可用，进入配网模式");
                creds_clear_all(app);
                device_set_state(app, DEV_STATE_AP_PROVISION);
            }
            return;
        }
        evlog_record(app, "WiFi 已连接：%s", app->creds[app->cred_active]);
        app->wifi_retry_count = 0;
        discovery_start(app);
        device_set_state(app, DEV_STATE_DISCOVERY);
        return;
    }
    if (reason == WIFI_REASON_AUTH_FAIL) {
        /* 不可恢复：切换下一凭据 */
        evlog_record(app, "WiFi 认证失败（202）：%s", app->creds[app->cred_active]);
        app->err_auth_fails++;
        app->cred_active++;
        app->wifi_retry_count = 0;
        if (app->cred_active >= app->cred_count) {
            evlog_record(app, "所有网络凭据均不可用，进入配网模式");
            creds_clear_all(app);
            device_set_state(app, DEV_STATE_AP_PROVISION);
        }
        return;
    }
    /* 可恢复：记录本次尝试时间，按指数退避等待下一轮 */
    app->wifi_retry_count++;
    if (app->wifi_retry_count >= app->params->wifi_retry_max) {
        evlog_record(app, "WiFi 重试次数耗尽（%d）：%s", (int)reason,
                     app->creds[app->cred_active]);
        app->err_wifi_disconnects++;
        app->cred_active++;
        app->wifi_retry_count = 0;
        if (app->cred_active >= app->cred_count) {
            evlog_record(app, "所有网络凭据均不可用，进入配网模式");
            creds_clear_all(app);
            device_set_state(app, DEV_STATE_AP_PROVISION);
        }
        return;
    }
    app->state_enter_ms = now; /* 退避起点：下一次尝试不早于 now+delay */
}

static void sm_ap_provision(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    /* PIN 锁定/失败退避中：等待退避结束转 BOOT */
    if (app->boot_backoff_until > now) {
        if (app->ap_listen == NULL && app->ap_conn == NULL)
            device_set_state(app, DEV_STATE_BOOT);
        return;
    }
    if (app->ap_listen == NULL) {
        int rc = prov_server_start(app);
        if (rc != DEMO_OK) {
            app->boot_backoff_until =
                now + (uint64_t)app->params->provision_ap_backoff_ms;
            evlog_record(app, "配网热点启动失败，等待 %d 毫秒后重试",
                         app->params->provision_ap_backoff_ms);
            device_set_state(app, DEV_STATE_BOOT);
            return;
        }
        app->state_enter_ms = now;
    }
    int prc = prov_server_poll(app);
    if (prc == PROV_RET_AP_LOCKED) {
        evlog_record(app, "配网热点因 PIN 连续失败而锁定");
        prov_server_stop(app);
        app->state_enter_ms = now;
        /* 热点退避 provision_ap_backoff_ms 后回 BOOT */
        app->boot_backoff_until = now + (uint64_t)app->params->provision_ap_backoff_ms;
        device_set_state(app, DEV_STATE_BOOT);
        return;
    }
    if (app->provision_wifi_ok && app->provision_handoff_deadline_ms > 0 &&
        now >= app->provision_handoff_deadline_ms) {
        LOG_I(app->device_id, "配网：未收到 close_ap，设备主动完成交接");
        evlog_record(app, "未收到 close_ap，设备主动完成配网交接");
        prov_server_stop(app);
    }
    /* 配网完成（wifi_config 成功 + close_ap 后 ap_listen 被置空且认证已通过）：
     * 必须同时满足 auth_ok 与 wifi_ok，防止未配置 WiFi 就被 close_ap 误入发现阶段 */
    if (app->ap_listen == NULL && app->provision_auth_ok) {
        if (!app->provision_wifi_ok) {
            /* 过早 close_ap（未完成 wifi_config）：视为配网失败，退避后重新开 AP */
            LOG_W(app->device_id, "配网：收到 close_ap 但未完成 wifi_config，视为配网失败");
            evlog_record(app, "配网中断（未完成 wifi_config）");
            app->boot_backoff_until = now + (uint64_t)app->params->provision_ap_backoff_ms;
            device_set_state(app, DEV_STATE_BOOT);
            return;
        }
        if (!ensure_expected_wifi_or_recover(app, "关闭热点前", 1))
            return;
        /* 写确认标记（配网确认窗口开始计时：由 SESSION 建立后确认） */
        evlog_record(app, "配网完成，进入服务发现阶段");
        discovery_start(app);
        device_set_state(app, DEV_STATE_DISCOVERY);
        return;
    }
    /* AP 空闲超时 → 关 AP 退避（配置为 0 表示持续开启：仅配网完成 close_ap 或 PIN 锁定才停止） */
    if (app->params->provision_ap_idle_timeout_ms > 0 &&
        app->ap_listen != NULL &&
        now - app->state_enter_ms >= (uint64_t)app->params->provision_ap_idle_timeout_ms) {
        evlog_record(app, "配网热点空闲超时");
        prov_server_stop(app);
        app->boot_backoff_until = now + (uint64_t)app->params->provision_ap_backoff_ms;
        device_set_state(app, DEV_STATE_BOOT);
    }
}

static void sm_discovery(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    if (!ensure_expected_wifi_or_recover(app, "服务发现前", 0))
        return;
    if (app->cred_count > 0 && !app->cred_confirmed[app->cred_active] &&
        app->provision_confirm_deadline_ms > 0 &&
        now >= app->provision_confirm_deadline_ms) {
        rollback_unconfirmed_credential(app, "确认窗口内未连接上位机");
        return;
    }
    int rc = discovery_poll(app);
    if (rc == 1) {
        evlog_record(app, "发现上位机：%u.%u.%u.%u:%u",
                     app->sess_host.ip & 0xFF, (app->sess_host.ip >> 8) & 0xFF,
                     (app->sess_host.ip >> 16) & 0xFF, (app->sess_host.ip >> 24) & 0xFF,
                     dev_ntohs(app->sess_host.port));
        device_set_state(app, DEV_STATE_CONNECT);
        return;
    }
    /* 看门狗（发现超长驻留） */
    if (now - app->state_enter_ms > (uint64_t)app->params->watchdog_state_timeout_ms) {
        LOG_W(app->device_id, "看门狗：服务发现阶段停留过久，重新开始发现");
        evlog_record(app, "服务发现阶段看门狗超时");
        discovery_start(app);
        app->state_enter_ms = now;
    }
}

static void sm_connect(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    if (!ensure_expected_wifi_or_recover(app, "连接上位机前", 0))
        return;
    if (app->cred_count > 0 && !app->cred_confirmed[app->cred_active] &&
        app->provision_confirm_deadline_ms > 0 && now >= app->provision_confirm_deadline_ms) {
        rollback_unconfirmed_credential(app, "确认窗口内未建立上位机会话");
        return;
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
    uint64_t now = net_time_ms(app->net);
    if (!ensure_expected_wifi_or_recover(app, "会话保活", 0))
        return;
    if (app->cred_count > 0 && !app->cred_confirmed[app->cred_active] &&
        app->provision_confirm_deadline_ms > 0 && now >= app->provision_confirm_deadline_ms) {
        rollback_unconfirmed_credential(app, "确认窗口内未收到 host_ack");
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
    /* 只有收到 host_ack status=ok 后，凭据才成为 last-known-good。 */
    if (app->session_ack_ok && app->cred_count > 0 &&
        !app->cred_confirmed[app->cred_active] && !app->cred_confirm_done) {
        app->cred_confirmed[app->cred_active] = 1;
        app->cred_confirm_done = 1;
        if (creds_save(app) == DEMO_OK) {
            app->provision_confirm_deadline_ms = 0;
            evlog_record(app, "网络凭据已确认（最近一次有效配置）");
        } else {
            app->cred_confirmed[app->cred_active] = 0;
            app->cred_confirm_done = 0;
            evlog_record(app, "网络凭据确认写入失败");
        }
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
    if (!ensure_expected_wifi_or_recover(app, "异常自愈", 0))
        return;
    if (app->cred_count > 0 && !app->cred_confirmed[app->cred_active] &&
        app->provision_confirm_deadline_ms > 0 && now >= app->provision_confirm_deadline_ms) {
        rollback_unconfirmed_credential(app, "确认窗口内上位机会话失败");
        return;
    }
    /* 退避到期后重连 */
    int delay = heal_next_backoff_ms(app);
    if (now - app->state_enter_ms >= (uint64_t)delay) {
        int rc = session_connect(app, &app->sess_host);
        if (rc == DEMO_OK) {
            evlog_record(app, "已重新连接上位机");
            heal_reset_backoff(app);
            device_set_state(app, DEV_STATE_SESSION);
            return;
        }
        /* 重连失败：重新计时退避 */
        app->state_enter_ms = now;
    }
    /* 长时间未成功 → 回发现 */
    if (now - app->heal_enter_ms >= (uint64_t)app->params->reconnect_to_discovery_ms) {
        evlog_record(app, "重连超时，返回服务发现阶段");
        discovery_start(app);
        device_set_state(app, DEV_STATE_DISCOVERY);
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
        case DEV_STATE_BOOT:         sm_boot(app); break;
        case DEV_STATE_STA_JOIN:     sm_sta_join(app); break;
        case DEV_STATE_AP_PROVISION: sm_ap_provision(app); break;
        case DEV_STATE_DISCOVERY:    sm_discovery(app); break;
        case DEV_STATE_CONNECT:      sm_connect(app); break;
        case DEV_STATE_SESSION:      sm_session(app); break;
        case DEV_STATE_HEAL:         sm_heal(app); break;
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
