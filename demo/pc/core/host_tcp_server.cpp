#include "host_tcp_server.h"
#include "serial_text_frame.h"
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

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

static std::string peer_text(const net_addr_t &peer)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&peer.ip);
    char text[32];
    snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", bytes[0], bytes[1], bytes[2], bytes[3],
             static_cast<unsigned>(ntohs(peer.port)));
    return text;
}

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

static bool ParseSerialProfile(const cJSON *json, HostSerialProfile *out)
{
    const cJSON *sp = cJSON_GetObjectItemCaseSensitive(json, "serial_profile");
    if (!cJSON_IsObject(sp))
        return false;
    const cJSON *fs = cJSON_GetObjectItemCaseSensitive(sp, "frame_size");
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(sp, "rows");
    const cJSON *cols = cJSON_GetObjectItemCaseSensitive(sp, "cols");
    const cJSON *dp = cJSON_GetObjectItemCaseSensitive(sp, "data_points");
    const cJSON *vd = cJSON_GetObjectItemCaseSensitive(sp, "value_domain");
    if (!cJSON_IsNumber(fs) || !cJSON_IsNumber(rows) || !cJSON_IsNumber(cols) ||
        !cJSON_IsNumber(dp) || !cJSON_IsString(vd))
        return false;
    if (strcmp(vd->valuestring, "raw_adc") != 0)
        return false;
    uint32_t frame_size = (uint32_t)fs->valueint;
    uint32_t data_points = (uint32_t)dp->valueint;
    uint32_t r = (uint32_t)rows->valueint;
    uint32_t c = (uint32_t)cols->valueint;
    if (frame_size < 6 || r == 0 || c == 0)
        return false;
    if (frame_size != 4 + data_points * 2)
        return false;
    if (r * c != data_points)
        return false;
    out->frame_size = frame_size;
    out->rows = (uint16_t)r;
    out->cols = (uint16_t)c;
    out->data_points = data_points;
    return true;
}

HostTcpServer::HostTcpServer(net_ctx_t *net, HostRegistry &reg, const demo_params_t &params,
                             IHostDataSink *sink)
    : net_(net), reg_(reg), params_(params), sink_(sink)
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
    if (sock == nullptr || obj == nullptr)
        return DEMO_ERR_INVAL;
    char *s = cJSON_PrintUnformatted(obj);
    if (!s)
        return DEMO_ERR_NOMEM;
    SendCtx &ctx = send_ctx_[sock];
    std::vector<uint8_t> frame((size_t)(PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN +
                                        strlen(s)));
    uint64_t seq = ++ctx.tx_sequence;
    int n = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, seq,
                          (const uint8_t *)s, (int)strlen(s),
                          frame.data(), (int)frame.size());
    if (n >= 0)
        LOG_I("HOST", "TX %s", s);
    free(s);
    if (n < 0)
        return n;
    frame.resize((size_t)n);
    ctx.queue.push_back(std::move(frame));
    return DEMO_OK;
}

int HostTcpServer::SendSerialBytes(void *sock, const uint8_t *bytes, size_t length)
{
    if (sock == nullptr || bytes == nullptr || length == 0 ||
        length > PROTO_V2_SERIAL_CHUNK_MAX)
        return DEMO_ERR_INVAL;
    SendCtx &ctx = send_ctx_[sock];
    std::vector<uint8_t> frame((size_t)(PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN +
                                        length));
    uint64_t seq = ++ctx.tx_sequence;
    int n = frame_v2_wrap(PROTO_V2_TYPE_SERIAL_BYTES, seq, bytes, (int)length,
                          frame.data(), (int)frame.size());
    if (n < 0)
        return n;
    frame.resize((size_t)n);
    ctx.queue.push_back(std::move(frame));
    return DEMO_OK;
}

void HostTcpServer::FlushTx()
{
    for (auto it = send_ctx_.begin(); it != send_ctx_.end();) {
        SendCtx &ctx = it->second;
        void *sock = it->first;
        bool fatal = false;

        if (!ctx.inflight.empty()) {
            int n = net_sock_send(net_, sock, ctx.inflight.data() + ctx.inflight_off,
                                  (int)(ctx.inflight.size() - ctx.inflight_off));
            if (n == DEMO_ERR_AGAIN) {
                ++it;
                continue;
            }
            if (n <= 0) {
                fatal = true;
            } else {
                ctx.inflight_off += (size_t)n;
                if (ctx.inflight_off >= ctx.inflight.size()) {
                    ctx.inflight.clear();
                    ctx.inflight_off = 0;
                } else {
                    ++it;
                    continue;
                }
            }
        }

        while (!fatal && !ctx.queue.empty()) {
            std::vector<uint8_t> frame = std::move(ctx.queue.front());
            ctx.queue.pop_front();
            int n = net_sock_send(net_, sock, frame.data(), (int)frame.size());
            if (n == DEMO_ERR_AGAIN) {
                ctx.inflight = std::move(frame);
                ctx.inflight_off = 0;
                break;
            }
            if (n <= 0) {
                fatal = true;
                break;
            }
            if ((size_t)n < frame.size()) {
                ctx.inflight = std::move(frame);
                ctx.inflight_off = (size_t)n;
                break;
            }
        }

        if (fatal) {
            net_sock_close(net_, sock);
            it = send_ctx_.erase(it);
            continue;
        }
        ++it;
    }
}

struct ParsedFrame {
    int type = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> payload;
};

static int parse_v2_frames(std::vector<uint8_t> &rx, ParsedFrame *out)
{
    int type = 0, plen = 0, consumed = 0;
    uint64_t seq = 0;
    const uint8_t *pp = nullptr;
    int rc = frame_v2_parse(rx.data(), (int)rx.size(), &type, &seq, &pp, &plen, &consumed);
    if (rc == 0)
        return 0;
    if (rc < 0) {
        if (consumed <= 0)
            rx.clear();
        else
            rx.erase(rx.begin(), rx.begin() + consumed);
        return -1;
    }
    out->type = type;
    out->sequence = seq;
    out->payload.assign(pp, pp + plen);
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
    FlushTx(); /* 尽力发送 busy ack 后再关闭 */
    send_ctx_.erase(conn);
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
        net_addr_t peer{};
        int rc = net_tcp_accept(net_, listen_, &conn, &peer);
        if (rc == DEMO_ERR_AGAIN)
            break;
        if (rc != DEMO_OK)
            break;
        std::string peer_name = peer_text(peer);
        if (ActiveConnCount() >= PROTO_HOST_MAX_CONN) {
            LOG_W("HOST", "检测到并发连接：peer=%s，当前连接数=%d", peer_name.c_str(),
                  ActiveConnCount());
            RejectBusy(conn);
        } else {
            PendingConn pc;
            pc.sock = conn;
            pc.peer = peer;
            pc.connect_ms = now_ms;
            pending_.push_back(pc);
            LOG_I("HOST", "已接受连接：peer=%s，等待 device_hello", peer_name.c_str());
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
        std::string session_id;
        if (e) {
            session_id = e->session_id;
            if (e->conn) {
                void *conn = e->conn;
                net_sock_close(net_, conn);
                send_ctx_.erase(conn);
                conn_rx_.erase(conn);
                e->conn = nullptr;
            }
        }
        reg_.MarkOffline(id);
        if (sink_)
            sink_->OnSessionOffline(id, session_id, "heartbeat_timeout");
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
    /* 冲刷 UI 入队的单帧串口消息（SERIAL_BYTES） */
    FlushPendingSerialTx();
    /* 冲刷发送队列（控制 + 数据，处理 partial write / EAGAIN） */
    FlushTx();
}

int HostTcpServer::QueueAppData(const std::string &device_id, const std::string &text)
{
    if (device_id.empty() || text.empty() || text.size() > APP_DATA_TEXT_MAX)
        return DEMO_ERR;
    std::lock_guard<std::mutex> lock(tx_mu_);
    pending_tx_.emplace_back(device_id, text);
    return DEMO_OK;
}

int HostTcpServer::QueueSerialFrame(const std::string &device_id,
                                    const std::vector<uint8_t> &frame)
{
    if (device_id.empty() || frame.empty() ||
        frame.size() > PROTO_V2_SERIAL_CHUNK_MAX)
        return DEMO_ERR;
    std::lock_guard<std::mutex> lock(tx_mu_);
    pending_serial_frames_.push_back({device_id, frame});
    return DEMO_OK;
}

void HostTcpServer::FlushPendingSerialTx()
{
    std::vector<PendingSerialFrame> queue;
    {
        std::lock_guard<std::mutex> lock(tx_mu_);
        queue.swap(pending_serial_frames_);
    }
    for (const auto &item : queue) {
        DeviceEntry *e = reg_.Find(item.device_id);
        if (!e || e->state != "online" || !e->conn) {
            LOG_W("HOST", "串口单帧丢弃：%s 不在线", item.device_id.c_str());
            continue;
        }

        LOG_I("HOST", "[串口TX] 单帧串口消息（%zu 字节）：%s",
              item.frame.size(),
              serial_text_frame::Hex(item.frame.data(), item.frame.size()).c_str());

        std::string unpacked;
        if (serial_text_frame::Decode(item.frame.data(), item.frame.size(),
                                      &unpacked)) {
            LOG_I("HOST", "[串口TX] 解包出来的串口消息（%zu 字节）：%s",
                  unpacked.size(), unpacked.c_str());
        } else {
            LOG_W("HOST", "[串口TX] 该帧无法按文本帧规则解包，仍按原始字节下发");
        }

        const int rc = SendSerialBytes(e->conn, item.frame.data(),
                                       item.frame.size());
        if (rc != DEMO_OK)
            LOG_W("HOST", "串口单帧发送失败：%s（rc=%d）",
                  item.device_id.c_str(), rc);
    }
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
        ParsedFrame frame;
        int rc = parse_v2_frames(pc.rx, &frame);
        if (rc == 0)
            break;
        if (rc < 0) {
            pc.malformed++;
            LOG_W("HOST", "待注册连接收到畸形帧（%d）", pc.malformed);
            continue;
        }
        if (frame.type != PROTO_V2_TYPE_CONTROL_JSON) {
            LOG_W("HOST", "握手前收到 SERIAL_BYTES，协议错误，关闭连接");
            net_sock_close(net_, pc.sock);
            pc.sock = nullptr;
            break;
        }
        cJSON *json = cJSON_ParseWithLength((const char *)frame.payload.data(),
                                            frame.payload.size());
        if (json == nullptr) {
            pc.malformed++;
            LOG_W("HOST", "控制帧 JSON 解析失败");
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
            if (!cJSON_IsString(id) || !cJSON_IsNumber(pv) || !cJSON_IsString(fw) ||
                !cJSON_IsNumber(upt)) {
                pc.malformed++;
                LOG_W("HOST", "device_hello 字段无效");
                cJSON_Delete(json);
                continue;
            }
            if (pv->valueint != PROTO_WIRE_VERSION) {
                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "cmd", CMD_HOST_ACK);
                cJSON_AddStringToObject(ack, "status", "fail");
                cJSON_AddStringToObject(ack, "reason", "unsupported_protocol");
                SendFrame(pc.sock, ack);
                cJSON_Delete(ack);
                LOG_W("HOST", "设备 %s 的 proto_ver=%d 不受支持，已拒绝",
                      id->valuestring, pv->valueint);
                FlushTx(); /* 尽力发送 fail ack 后再关闭 */
                send_ctx_.erase(pc.sock);
                net_sock_close(net_, pc.sock);
                pc.sock = nullptr;
                cJSON_Delete(json);
                break;
            }
            HostSerialProfile profile;
            if (!ParseSerialProfile(json, &profile)) {
                cJSON *ack = cJSON_CreateObject();
                cJSON_AddStringToObject(ack, "cmd", CMD_HOST_ACK);
                cJSON_AddStringToObject(ack, "status", "fail");
                cJSON_AddStringToObject(ack, "reason", "invalid_serial_profile");
                SendFrame(pc.sock, ack);
                cJSON_Delete(ack);
                LOG_W("HOST", "设备 %s 的 serial_profile 非法，已拒绝", id->valuestring);
                FlushTx(); /* 尽力发送 fail ack 后再关闭 */
                send_ctx_.erase(pc.sock);
                net_sock_close(net_, pc.sock);
                pc.sock = nullptr;
                cJSON_Delete(json);
                break;
            }
            DeviceEntry *old = reg_.Find(id->valuestring);
            bool was_offline = old && old->state == "offline";
            bool resumed = false;
            DeviceEntry *e = reg_.Add(id->valuestring, pc.sock);
            e->fw_version = fw->valuestring;
            e->proto_ver = pv->valueint;
            if (was_offline) {
                e->reconnect_count = old->reconnect_count + 1;
                if (cJSON_IsString(sid) && !old->session_id.empty() &&
                    strcmp(sid->valuestring, old->session_id.c_str()) == 0)
                    resumed = true;
            }
            if (!resumed || e->session_id.empty())
                e->session_id = NewSessionId();
            reg_.OnRx(id->valuestring, now_ms);

            cJSON *ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "cmd", CMD_HOST_ACK);
            cJSON_AddStringToObject(ack, "status", "ok");
            cJSON_AddNumberToObject(ack, "heartbeat_interval", params_.heartbeat_interval_ms / 1000);
            cJSON_AddStringToObject(ack, "session_id", e->session_id.c_str());
            cJSON_AddNumberToObject(ack, "proto_ver", PROTO_WIRE_VERSION);
            SendFrame(pc.sock, ack);
            cJSON_Delete(ack);
            LOG_I("HOST", "设备注册成功：%s，peer=%s，session_id=%s%s", id->valuestring,
                  peer_text(pc.peer).c_str(), e->session_id.c_str(),
                  resumed ? "（已恢复）" : "");
            e->conn = pc.sock; /* 从 pending 转入 registry */
            pc.sock = nullptr;
            {
                std::lock_guard<std::mutex> sink_lock(sink_mu_);
                if (sink_)
                    sink_->OnSessionOnline(id->valuestring, e->session_id, profile);
            }
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
        if (name) {
            LOG_D("HOST", "待注册连接收到未知命令 %s（已忽略）", name);
        }
        cJSON_Delete(json);
    }
}

void HostTcpServer::HandleOnline(DeviceEntry &e, uint64_t now_ms)
{
    static constexpr size_t kReceiveBudget = 256 * 1024;
    uint8_t buf[4096];
    size_t received = 0;

    while (received < kReceiveBudget) {
        int n = net_sock_recv(net_, e.conn, buf, (int)sizeof(buf));
        if (n == DEMO_ERR_AGAIN)
            return;
        if (n <= 0) {
            LOG_W("HOST", "设备连接已断开：%s（接收返回=%d）", e.id.c_str(), n);
            net_sock_close(net_, e.conn);
            conn_rx_.erase(e.conn);
            send_ctx_.erase(e.conn);
            e.conn = nullptr;
            reg_.MarkOffline(e.id);
            {
                std::lock_guard<std::mutex> sink_lock(sink_mu_);
                if (sink_)
                    sink_->OnSessionOffline(e.id, e.session_id, "conn_lost");
            }
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
        received += (size_t)n;
        LOG_D("HOST", "从在线设备 %s 收到 %d 字节", e.id.c_str(), n);
        std::vector<uint8_t> &rx = conn_rx_[e.conn];
        rx.insert(rx.end(), buf, buf + n);
        while (!rx.empty()) {
            ParsedFrame frame;
            int rc = parse_v2_frames(rx, &frame);
            if (rc == 0)
                break;
            if (rc < 0) {
                LOG_W("HOST", "在线连接收到畸形帧，已丢弃");
                continue;
            }
            reg_.OnRx(e.id, now_ms);
            if (frame.type == PROTO_V2_TYPE_SERIAL_BYTES) {

                std::string unpacked;
                if (serial_text_frame::Decode(frame.payload.data(),
                                              frame.payload.size(), &unpacked)) {
                    LOG_I("HOST", "[串口RX] 单帧串口消息（%zu 字节）：%s",
                          frame.payload.size(),
                          serial_text_frame::Hex(frame.payload.data(),
                                                 frame.payload.size()).c_str());
                    LOG_I("HOST", "[串口RX] 解包出来的串口消息（%zu 字节）：%s",
                          unpacked.size(), unpacked.c_str());
                }
                {
                    std::lock_guard<std::mutex> sink_lock(sink_mu_);
                    if (sink_)
                        sink_->OnSerialBytes(e.id, e.session_id, frame.payload.data(),
                                             frame.payload.size(), now_ms);
                }
                continue;
            }
            cJSON *json = cJSON_ParseWithLength((const char *)frame.payload.data(),
                                                frame.payload.size());
            if (json == nullptr) {
                LOG_W("HOST", "在线控制帧 JSON 解析失败");
                continue;
            }
            const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
            const char *name = cmd && cJSON_IsString(cmd) ? cmd->valuestring : nullptr;
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
                }
            } else if (name) {
                LOG_D("HOST", "在线连接收到未知命令：%s", name);
            }
            cJSON_Delete(json);
        }
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
