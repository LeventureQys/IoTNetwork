#include "host_tcp_server.h"
#include "cJSON.h"
#include "frame.h"
#include "protocol.h"
#include "log.h"
#include "pc_event.h"
#include "pc_sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>

static void emit_app_data_event(const char *event, const char *device_id, const char *text)
{
    char digest[65];
    pc_sha256_hex((const uint8_t *)text, strlen(text), digest);
    char data[160];
    snprintf(data, sizeof(data), "{\"text_bytes\":%zu,\"sha256\":\"%s\"}", strlen(text), digest);
    pc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = event;
    ev.result = "ok";
    ev.code = DEMO_OK;
    ev.device_id = device_id;
    ev.data_json = data;
    pc_events_emit(&ev);
}

HostTcpServer::HostTcpServer(net_ctx_t *net, HostRegistry &reg, const demo_params_t &params)
    : net_(net), reg_(reg), params_(params)
{
}

HostTcpServer::~HostTcpServer()
{
    Stop();
}

int HostTcpServer::Start()
{
    if (listen_)
        return DEMO_OK;
    int rc = net_tcp_listen(net_, (uint16_t)params_.host_tcp_port, &listen_);
    if (rc != DEMO_OK) {
        LOG_E("HOST", "TCP 服务监听失败，端口=%d", params_.host_tcp_port);
        return rc;
    }
    LOG_I("HOST", "TCP 服务正在监听 0.0.0.0:%d（监听端点，非通告地址）",
          params_.host_tcp_port);
    return DEMO_OK;
}

void HostTcpServer::Stop()
{
    BroadcastCloseAll();
    if (listen_) {
        net_sock_close(net_, listen_);
        listen_ = nullptr;
    }
}

std::string HostTcpServer::NewSessionId()
{
    static std::mt19937 rng((uint32_t)time(nullptr) ^ 0x5EED);
    char buf[PROTO_SESSION_ID_LEN + 1];
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < PROTO_SESSION_ID_LEN; i++)
        buf[i] = hex[rng() % 16];
    buf[PROTO_SESSION_ID_LEN] = '\0';
    return buf;
}

int HostTcpServer::SendFrame(void *sock, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s)
        return DEMO_ERR_NOMEM;
    uint8_t frame[PROTO_MSG_MAX_LEN + PROTO_FRAME_HEAD_LEN];
    int n = frame_wrap((const uint8_t *)s, (int)strlen(s), frame, (int)sizeof(frame));
    int rc = n < 0 ? n : net_sock_send(net_, sock, frame, n);
    if (rc >= 0)
        LOG_I("HOST", "TX %s", s);
    free(s);
    return rc >= 0 ? DEMO_OK : rc;
}

static int parse_frames(std::vector<uint8_t> &rx, cJSON **out)
{
    int off = 0, len = 0, consumed = 0;
    int rc = frame_parse(rx.data(), (int)rx.size(), &off, &len, &consumed);
    if (rc == 0)
        return 0;
    if (rc == DEMO_ERR) {
        rx.erase(rx.begin(), rx.begin() + consumed);
        return -1;
    }
    std::string payload((const char *)(rx.data() + off), (size_t)len);
    *out = cJSON_Parse(payload.c_str());
    rx.erase(rx.begin(), rx.begin() + consumed);
    return 1;
}

int HostTcpServer::ActiveConnCount() const
{
    int count = (int)pending_.size();
    for (DeviceEntry *e : reg_.AllOnline())
        if (e->conn)
            count++;
    return count;
}

void HostTcpServer::RejectBusy(void *conn)
{
    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "cmd", CMD_HOST_ACK);
    cJSON_AddStringToObject(ack, "status", "busy");
    cJSON_AddStringToObject(ack, "reason", "single_device_only");
    SendFrame(conn, ack);
    cJSON_Delete(ack);
    LOG_W("HOST", "第二连接被拒绝：single_device_only");
    net_sock_close(net_, conn);
    pc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = "single_device_rejected";
    ev.result = "ok";
    ev.code = DEMO_OK;
    ev.data_json = "{\"reason\":\"single_device_only\"}";
    pc_events_emit(&ev);
}

void HostTcpServer::Poll(uint64_t now_ms)
{
    if (!listen_)
        return;

    /* accept 新连接（固定一对一：在线 + pending 总数 ≤ 1） */
    while (true) {
        void *conn = nullptr;
        int rc = net_tcp_accept(net_, listen_, &conn, nullptr);
        if (rc == DEMO_ERR_AGAIN)
            break;
        if (rc != DEMO_OK)
            break;
        if (ActiveConnCount() >= PROTO_HOST_MAX_CONN) {
            RejectBusy(conn);
        } else {
            PendingConn pc;
            pc.sock = conn;
            pc.connect_ms = now_ms;
            pending_.push_back(pc);
            LOG_I("HOST", "已接受连接，等待 device_hello");
        }
    }

    /* pending 连接（等 device_hello） */
    for (auto it = pending_.begin(); it != pending_.end();) {
        HandlePending(*it, now_ms);
        if (it->sock == nullptr) {
            it = pending_.erase(it);
        } else if (now_ms - it->connect_ms > (uint64_t)params_.hello_timeout_ms ||
                   it->malformed >= params_.malformed_max_per_conn) {
            LOG_W("HOST", "待注册连接超时或报文畸形，正在关闭");
            net_sock_close(net_, it->sock);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }

    /* 已注册连接（心跳等） */
    for (DeviceEntry *e : reg_.AllOnline()) {
        if (e->conn)
            HandleOnline(*e, now_ms);
        else
            reg_.MarkOffline(e->id); /* 连接已断开（FIN/对端关闭）→ 立即离线 */
    }

    /* 心跳判死 */
    int dead_ms = params_.heartbeat_dead_ms > 0 ? params_.heartbeat_dead_ms
                                                : params_.heartbeat_interval_ms * 3 / 2;
    for (const std::string &id : reg_.FindDead(now_ms, dead_ms)) {
        DeviceEntry *e = reg_.Find(id);
        LOG_W("HOST", "设备因心跳超时离线：%s", id.c_str());
        if (e && e->conn) {
            net_sock_close(net_, e->conn);
            e->conn = nullptr;
        }
        reg_.MarkOffline(id);
        pc_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = "session_offline";
        ev.result = "ok";
        ev.code = DEMO_OK;
        ev.device_id = id.c_str();
        ev.data_json = "{\"reason\":\"heartbeat_timeout\"}";
        pc_events_emit(&ev);
    }

    /* 冲刷 UI 入队的联调消息（app_data） */
    FlushPendingTx();
}

int HostTcpServer::QueueAppData(const std::string &device_id, const std::string &text)
{
    if (device_id.empty() || text.empty() || text.size() > APP_DATA_TEXT_MAX)
        return DEMO_ERR;
    std::lock_guard<std::mutex> lock(tx_mu_);
    pending_tx_.emplace_back(device_id, text);
    return DEMO_OK;
}

void HostTcpServer::FlushPendingTx()
{
    std::vector<std::pair<std::string, std::string>> queue;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        queue.swap(pending_tx_);
    }
    for (const auto &item : queue) {
        DeviceEntry *e = reg_.Find(item.first);
        if (!e || e->state != "online" || !e->conn) {
            LOG_W("HOST", "调试消息丢弃：%s 不在线", item.first.c_str());
            continue;
        }
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "cmd", CMD_APP_DATA);
        cJSON_AddNumberToObject(msg, "seq", (int)++app_data_seq_);
        cJSON *data = cJSON_AddObjectToObject(msg, "data");
        cJSON_AddStringToObject(data, "type", "debug_text");
        cJSON_AddStringToObject(data, "text", item.second.c_str());
        int rc = SendFrame(e->conn, msg);
        cJSON_Delete(msg);
        if (rc == DEMO_OK) {
            LOG_I("HOST", "已向 %s 发送调试消息（%zu 字节）", item.first.c_str(),
                  item.second.size());
            emit_app_data_event("app_data_tx", item.first.c_str(), item.second.c_str());
        } else
            LOG_W("HOST", "调试消息发送失败：%s（rc=%d）", item.first.c_str(), rc);
    }
}

void HostTcpServer::HandlePending(PendingConn &pc, uint64_t now_ms)
{
    uint8_t buf[512];
    int n = net_sock_recv(net_, pc.sock, buf, (int)sizeof(buf));
    if (n == DEMO_ERR_AGAIN)
        return;
    if (n <= 0) {
        net_sock_close(net_, pc.sock);
        pc.sock = nullptr;
        return;
    }
    pc.rx.insert(pc.rx.end(), buf, buf + n);
    while (!pc.rx.empty()) {
        cJSON *json = nullptr;
        int rc = parse_frames(pc.rx, &json);
        if (rc == 0)
            break;
        if (rc == DEMO_ERR) {
            pc.malformed++;
            LOG_W("HOST", "待注册连接收到畸形帧（%d）", pc.malformed);
            continue;
        }
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
        const char *name = cmd && cJSON_IsString(cmd) ? cmd->valuestring : nullptr;
        if (name && strcmp(name, CMD_DEVICE_HELLO) == 0) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
            const cJSON *sid = cJSON_GetObjectItemCaseSensitive(json, "session_id");
            const cJSON *pv = cJSON_GetObjectItemCaseSensitive(json, "proto_ver");
            const cJSON *fw = cJSON_GetObjectItemCaseSensitive(json, "fw_version");
            const cJSON *upt = cJSON_GetObjectItemCaseSensitive(json, "uptime");
            /* beta v1.1：必需字段 id/fw_version/proto_ver/uptime；session_id 可选 */
            if (!cJSON_IsString(id) || !cJSON_IsNumber(pv) || !cJSON_IsString(fw) ||
                !cJSON_IsNumber(upt)) {
                pc.malformed++;
                LOG_W("HOST", "device_hello 字段无效");
                cJSON_Delete(json);
                continue;
            }
            /* 协议版本协商 */
            if (pv->valueint != PROTO_VERSION) {
                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "cmd", CMD_HOST_ACK);
                cJSON_AddStringToObject(ack, "status", "fail");
                cJSON_AddStringToObject(ack, "reason", "unsupported_protocol");
                SendFrame(pc.sock, ack);
                cJSON_Delete(ack);
                LOG_W("HOST", "设备 %s 的 proto_ver=%d 不受支持，已拒绝",
                      id->valuestring, pv->valueint);
                net_sock_close(net_, pc.sock);
                pc.sock = nullptr;
                cJSON_Delete(json);
                break;
            }
            /* 注册处理：离线同 ID 可恢复；在线同 ID 已在 accept 阶段按 busy 拒绝 */
            DeviceEntry *old = reg_.Find(id->valuestring);
            bool was_offline = old && old->state == "offline";
            bool resumed = false;
            DeviceEntry *e = reg_.Add(id->valuestring, pc.sock);
            e->fw_version = fw->valuestring;
            e->proto_ver = pv->valueint;
            if (was_offline) {
                e->reconnect_count = old->reconnect_count + 1;
                LOG_I("HOST", "注册：进入会话恢复路径，旧重连次数=%d，新重连次数=%d，session_id=%s",
                      old->reconnect_count, e->reconnect_count,
                      cJSON_IsString(sid) ? sid->valuestring : "（无）");
                if (cJSON_IsString(sid) && !old->session_id.empty() &&
                    strcmp(sid->valuestring, old->session_id.c_str()) == 0) {
                    LOG_I("HOST", "会话已恢复：%s", id->valuestring);
                    resumed = true;
                }
            } else {
                LOG_I("HOST", "注册：进入新会话路径，旧状态=%s",
                      old ? old->state.c_str() : "（无）");
            }
            if (!resumed || e->session_id.empty())
                e->session_id = NewSessionId();
            reg_.OnRx(id->valuestring, now_ms);

            cJSON *ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "cmd", CMD_HOST_ACK);
            cJSON_AddStringToObject(ack, "status", "ok");
            cJSON_AddNumberToObject(ack, "heartbeat_interval", params_.heartbeat_interval_ms / 1000);
            cJSON_AddStringToObject(ack, "session_id", e->session_id.c_str());
            cJSON_AddNumberToObject(ack, "proto_ver", PROTO_VERSION);
            SendFrame(pc.sock, ack);
            cJSON_Delete(ack);
            LOG_I("HOST", "设备注册成功：%s，session_id=%s%s", id->valuestring,
                  e->session_id.c_str(), resumed ? "（已恢复）" : "");
            e->conn = pc.sock; /* 从 pending 转入 registry */
            pc.sock = nullptr;
            {
                char data[128];
                snprintf(data, sizeof(data), "{\"peer\":\"%s\",\"reconnect_count\":%d}",
                         id->valuestring, e->reconnect_count);
                pc_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.event = "session_online";
                ev.result = "ok";
                ev.code = DEMO_OK;
                ev.device_id = id->valuestring;
                ev.data_json = data;
                pc_events_emit(&ev);
            }
            cJSON_Delete(json);
            break;
        }
        /* 未知命令（协议文档 10.2 前向兼容） */
        if (name) {
            LOG_D("HOST", "待注册连接收到未知命令 %s（已忽略）", name);
        }
        cJSON_Delete(json);
    }
}

void HostTcpServer::HandleOnline(DeviceEntry &e, uint64_t now_ms)
{
    uint8_t buf[512];
    int n = net_sock_recv(net_, e.conn, buf, (int)sizeof(buf));
    if (n == DEMO_ERR_AGAIN)
        return;
    if (n <= 0) {
        LOG_W("HOST", "设备连接已断开：%s（接收返回=%d）", e.id.c_str(), n);
        net_sock_close(net_, e.conn);
        conn_rx_.erase(e.conn);
        e.conn = nullptr;
        reg_.MarkOffline(e.id);
        pc_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = "session_offline";
        ev.result = "ok";
        ev.code = DEMO_OK;
        ev.device_id = e.id.c_str();
        ev.data_json = "{\"reason\":\"conn_lost\"}";
        pc_events_emit(&ev);
        return;
    }
    LOG_D("HOST", "从在线设备 %s 收到 %d 字节", e.id.c_str(), n);
    std::vector<uint8_t> &rx = conn_rx_[e.conn];
    rx.insert(rx.end(), buf, buf + n);
    while (!rx.empty()) {
        cJSON *json = nullptr;
        int rc = parse_frames(rx, &json);
        if (rc == 0)
            break;
        if (rc == DEMO_ERR) {
            LOG_W("HOST", "在线连接收到畸形帧，已丢弃");
            continue;
        }
        const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
        const char *name = cmd && cJSON_IsString(cmd) ? cmd->valuestring : nullptr;
        reg_.OnRx(e.id, now_ms);
        if (name && strcmp(name, CMD_PING) == 0) {
            const cJSON *seq = cJSON_GetObjectItemCaseSensitive(json, "seq");
            if (cJSON_IsNumber(seq)) {
                reg_.OnPing(e.id, (uint32_t)seq->valueint);
                {
                    char data[96];
                    snprintf(data, sizeof(data), "{\"direction\":\"rx\",\"sequence\":%d}",
                             seq->valueint);
                    pc_event_t ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.event = "ping";
                    ev.result = "ok";
                    ev.code = DEMO_OK;
                    ev.device_id = e.id.c_str();
                    ev.data_json = data;
                    pc_events_emit(&ev);
                }
                cJSON *pong = cJSON_CreateObject();
                cJSON_AddStringToObject(pong, "cmd", CMD_PONG);
                cJSON_AddNumberToObject(pong, "seq", seq->valueint); /* 回显序号 */
                SendFrame(e.conn, pong);
                cJSON_Delete(pong);
                {
                    char data[96];
                    snprintf(data, sizeof(data), "{\"direction\":\"tx\",\"sequence\":%d}",
                             seq->valueint);
                    pc_event_t ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.event = "pong";
                    ev.result = "ok";
                    ev.code = DEMO_OK;
                    ev.device_id = e.id.c_str();
                    ev.data_json = data;
                    pc_events_emit(&ev);
                }
            }
        } else if (name && strcmp(name, CMD_APP_DATA) == 0) {
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
            const cJSON *text = data ? cJSON_GetObjectItemCaseSensitive(data, "text") : nullptr;
            if (cJSON_IsString(text)) {
                LOG_I("HOST", "收到来自 %s 的调试消息：%s", e.id.c_str(), text->valuestring);
                emit_app_data_event("app_data_rx", e.id.c_str(), text->valuestring);
            } else
                LOG_I("HOST", "收到来自 %s 的 app_data 占位消息（无 text 字段）", e.id.c_str());
        } else if (name) {
            LOG_D("HOST", "在线连接收到未知命令：%s", name);
        }
        cJSON_Delete(json);
    }
}

void HostTcpServer::BroadcastCloseAll()
{
    for (DeviceEntry *e : reg_.AllOnline()) {
        if (e->conn) {
            net_sock_close(net_, e->conn);
            e->conn = nullptr;
        }
        reg_.MarkOffline(e->id);
    }
    for (PendingConn &pc : pending_) {
        if (pc.sock) {
            net_sock_close(net_, pc.sock);
            pc.sock = nullptr;
        }
    }
    pending_.clear();
    conn_rx_.clear();
}
