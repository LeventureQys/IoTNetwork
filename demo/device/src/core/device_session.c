#include "device_session.h"
#include "device_app.h"
#include "device_eventlog.h"
#include "device_limits.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ipv4_str(uint32_t ip, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

int session_connect(device_app_t *app, const net_addr_t *host)
{
    void *sock = NULL;
    int rc = net_tcp_connect(app->net, host, &sock, app->params->hello_timeout_ms);
    if (rc != DEMO_OK)
        return rc;
    app->sess_sock = sock;
    app->last_rx_ms = net_time_ms(app->net);
    app->last_ping_ms = 0;
    app->ping_seq = 0;
    app->malformed_count = 0;
    app->rx_len = 0; /* 新连接重置帧缓冲（跨连接残留会错乱解析） */

    /* device_hello（beta v1.1：仅 id/fw_version/proto_ver/uptime + 可选 session_id） */
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "cmd", CMD_DEVICE_HELLO);
    cJSON_AddStringToObject(hello, "id", app->device_id);
    cJSON_AddStringToObject(hello, "fw_version", app->params->device_fw_version);
    cJSON_AddNumberToObject(hello, "proto_ver", app->params->device_proto_ver);
    cJSON_AddNumberToObject(hello, "uptime", device_app_uptime_s(app));
    if (app->session_id[0] != '\0')
        cJSON_AddStringToObject(hello, "session_id", app->session_id);
    rc = device_send_frame(app, sock, hello);
    cJSON_Delete(hello);
    if (rc != DEMO_OK) {
        net_sock_close(app->net, sock);
        app->sess_sock = NULL;
        return rc;
    }
    app->session_connect_count++;
    LOG_I(app->device_id, "会话：已发送 device_hello（session_id=%s）", app->session_id);
    return DEMO_OK;
}

static void session_publish_online(device_app_t *app)
{
    char data[128];
    char peer[16];
    ipv4_str(app->sess_host.ip, peer, sizeof(peer));
    snprintf(data, sizeof(data), "{\"peer\":\"%s\",\"reconnect_count\":%u}",
             peer, app->session_connect_count);
    device_app_publish_event(app, "session_online", "ok", 0, data);
}

static void session_send_ping(device_app_t *app)
{
    if (app->sess_sock == NULL)
        return;
    if (!limits_allow_send(app, 1))
        return;
    cJSON *ping = cJSON_CreateObject();
    cJSON_AddStringToObject(ping, "cmd", CMD_PING);
    cJSON_AddNumberToObject(ping, "seq", (int)++app->ping_seq);
    int rc = device_send_frame(app, app->sess_sock, ping);
    if (rc == DEMO_OK) {
        char data[64];
        snprintf(data, sizeof(data), "{\"direction\":\"tx\",\"sequence\":%d}",
                 (int)app->ping_seq);
        device_app_publish_event(app, "ping", "ok", 0, data);
    }
    cJSON_Delete(ping);
}

int session_poll(device_app_t *app)
{
    if (app->sess_sock == NULL) {
        LOG_W(app->device_id, "会话：轮询时连接为空");
        return 0;
    }
    uint64_t now = net_time_ms(app->net);

    /* 收包 */
    device_app_handle_rx(app, app->sess_sock);

    /* 心跳 */
    if (app->last_ping_ms == 0 || now - app->last_ping_ms >= (uint64_t)app->heartbeat_interval_ms) {
        session_send_ping(app);
        app->last_ping_ms = net_time_ms(app->net);
    }

    /* 判死（重新取当前时间，避免 last_rx 在本轮刷新导致无符号下溢） */
    uint64_t now2 = net_time_ms(app->net);
    if (now2 - app->last_rx_ms > (uint64_t)app->heartbeat_dead_ms) {
        LOG_W(app->device_id, "会话：心跳超时（%llu 毫秒未收到数据）",
              (unsigned long long)(now2 - app->last_rx_ms));
        session_disconnect(app);
        return 0;
    }
    return 1;
}

void session_on_msg(device_app_t *app, cJSON *msg)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(msg, "cmd");
    if (!cmd || !cJSON_IsString(cmd))
        return;
    const char *name = cmd->valuestring;
    app->last_rx_ms = net_time_ms(app->net);

    if (strcmp(name, CMD_HOST_ACK) == 0) {
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(msg, "status");
        if (!cJSON_IsString(status))
            return;
        if (strcmp(status->valuestring, "ok") == 0) {
            app->session_ack_ok = 1;
            const cJSON *sid = cJSON_GetObjectItemCaseSensitive(msg, "session_id");
            const cJSON *hb = cJSON_GetObjectItemCaseSensitive(msg, "heartbeat_interval");
            const cJSON *pv = cJSON_GetObjectItemCaseSensitive(msg, "proto_ver");
            if (cJSON_IsString(sid))
                snprintf(app->session_id, sizeof(app->session_id), "%s", sid->valuestring);
            if (cJSON_IsNumber(hb) && hb->valueint > 0) {
                app->heartbeat_interval_ms = hb->valueint * 1000;
                app->heartbeat_dead_ms = app->params->heartbeat_dead_ms > 0
                    ? app->params->heartbeat_dead_ms
                    : app->heartbeat_interval_ms * 3 / 2;
            }
            LOG_I(app->device_id, "会话：host_ack 成功，session_id=%s，心跳=%d毫秒，proto_ver=%d",
                  app->session_id, app->heartbeat_interval_ms,
                  cJSON_IsNumber(pv) ? pv->valueint : -1);
            evlog_record(app, "会话已建立");
            session_publish_online(app);
        } else if (strcmp(status->valuestring, "busy") == 0) {
            LOG_W(app->device_id, "会话：上位机繁忙，进入长退避");
            evlog_record(app, "上位机繁忙");
            app->busy_pending = 1;
            session_disconnect(app);
        } else {
            const cJSON *reason = cJSON_GetObjectItemCaseSensitive(msg, "reason");
            LOG_W(app->device_id, "会话：host_ack 失败（%s）",
                  cJSON_IsString(reason) ? reason->valuestring : "未知原因");
            evlog_record(app, "host_ack 失败");
            session_disconnect(app);
        }
        return;
    }

    if (strcmp(name, CMD_PONG) == 0) {
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(msg, "seq");
        int s = cJSON_IsNumber(seq) ? (int)seq->valueint : 0;
        if (s != 0 && s != (int)app->ping_seq)
            LOG_W(app->device_id, "会话：pong 序号不匹配（期望=%u，实际=%d）",
                  app->ping_seq, s);
        char data[64];
        snprintf(data, sizeof(data), "{\"direction\":\"rx\",\"sequence\":%d}", s);
        device_app_publish_event(app, "pong", "ok", 0, data);
        LOG_T(app->device_id, "会话：收到 pong");
        return;
    }

    if (strcmp(name, CMD_APP_DATA) == 0) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(msg, "data");
        const cJSON *text = data ? cJSON_GetObjectItemCaseSensitive(data, "text") : NULL;
        if (cJSON_IsString(text)) {
            char ev[128];
            char sha[65];
            device_app_sha256_hex(text->valuestring, strlen(text->valuestring), sha);
            snprintf(ev, sizeof(ev), "{\"text_bytes\":%d,\"sha256\":\"%s\"}",
                     (int)strlen(text->valuestring), sha);
            device_app_publish_event(app, "app_data_rx", "ok", 0, ev);
            LOG_I(app->device_id, "会话：收到上位机调试消息：%s", text->valuestring);
        } else
            LOG_I(app->device_id, "会话：收到 app_data 占位消息（无 text 字段）");
        return;
    }

    /* 对端发起方向消息（hello/ping 等）属协议错误，由 handle_rx 计数畸形 */
    LOG_D(app->device_id, "会话：收到非预期命令 %s", name);
}

void session_on_conn_closed(device_app_t *app)
{
    if (app->sess_sock) {
        net_sock_close(app->net, app->sess_sock);
        app->sess_sock = NULL;
        app->err_tcp_drops++;
        LOG_W(app->device_id, "会话：连接已被对端关闭");
    }
}

void session_disconnect(device_app_t *app)
{
    if (app->sess_sock) {
        net_sock_close(app->net, app->sess_sock);
        app->sess_sock = NULL;
    }
}
