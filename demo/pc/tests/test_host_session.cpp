#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "cJSON.h"
#include "frame.h"
#include "protocol.h"
#include "host_tcp_server.h"
#include "host_registry.h"
#include "sim_backend.h"
#include "params.h"
#include "pc_event.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace {

const int kPort = 55956;

int v2_send_control(net_ctx_t *ctx, void *sock, uint64_t seq, const char *json)
{
    uint8_t frame[PROTO_V2_HEAD_LEN + PROTO_V2_BODY_HEAD_LEN + 2048];
    int n = frame_v2_wrap(PROTO_V2_TYPE_CONTROL_JSON, seq, (const uint8_t *)json,
                          (int)strlen(json), frame, (int)sizeof(frame));
    if (n < 0)
        return n;
    return net_sock_send(ctx, sock, frame, n);
}

bool v2_recv_control(net_ctx_t *ctx, void *sock, std::string *out, int timeout_ms)
{
    std::vector<uint8_t> rx;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t buf[512];
        int n = net_sock_recv(ctx, sock, buf, (int)sizeof(buf));
        if (n == DEMO_ERR_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (n <= 0)
            return false;
        rx.insert(rx.end(), buf, buf + n);
        while (!rx.empty()) {
            int type = 0, plen = 0, consumed = 0;
            uint64_t seq = 0;
            const uint8_t *pp = nullptr;
            int r = frame_v2_parse(rx.data(), (int)rx.size(), &type, &seq, &pp, &plen,
                                   &consumed);
            if (r == 0)
                break;
            if (r < 0)
                return false;
            if (type == PROTO_V2_TYPE_CONTROL_JSON) {
                out->assign(reinterpret_cast<const char *>(pp), (size_t)plen);
                return true;
            }
            rx.erase(rx.begin(), rx.begin() + consumed);
        }
    }
    return false;
}

struct SessionFixture {
    demo_params_t params;
    void *host_user = nullptr;
    void *dev_user = nullptr;
    net_ctx_t *host_ctx = nullptr;
    net_ctx_t *dev_ctx = nullptr;
    HostRegistry registry;
    HostTcpServer *server = nullptr;
    uint64_t now = 1000;
    uint64_t dev_seq = 0;

    int SendControl(void *sock, const char *json)
    {
        return v2_send_control(dev_ctx, sock, ++dev_seq, json);
    }

    bool RecvControl(void *sock, std::string *out, int timeout_ms)
    {
        return v2_recv_control(dev_ctx, sock, out, timeout_ms);
    }

    void Init()
    {
        params_defaults(&params);
        params.host_tcp_port = kPort;
        params.hello_timeout_ms = 5000;
        params.malformed_max_per_conn = 3;
        host_user = sim_backend_create("host", &params);
        dev_user = sim_backend_create("dev0", &params);
        net_ctx_t *hc = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), host_user, nullptr, &hc), DEMO_OK);
        host_ctx = hc;
        net_ctx_t *dc = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), dev_user, nullptr, &dc), DEMO_OK);
        dev_ctx = dc;
        server = new HostTcpServer(host_ctx, registry, params);
        ASSERT_EQ(server->Start(), DEMO_OK);
    }

    void Cleanup()
    {
        if (server) {
            server->Stop();
            delete server;
            server = nullptr;
        }
        if (host_ctx) net_ctx_destroy(host_ctx);
        if (dev_ctx) net_ctx_destroy(dev_ctx);
        if (host_user) sim_backend_destroy(host_user);
        if (dev_user) sim_backend_destroy(dev_user);
        host_ctx = dev_ctx = nullptr;
        host_user = dev_user = nullptr;
    }

    void *ConnectClient()
    {
        void *sock = nullptr;
        net_addr_t addr;
        addr.ip = htonl(INADDR_LOOPBACK);
        addr.port = htons(kPort);
        if (net_tcp_connect(dev_ctx, &addr, &sock, 2000) != DEMO_OK)
            return nullptr;
        return sock;
    }

    /* 握手并返回 ack JSON；session_id 可选（用于恢复） */
    std::string Handshake(void *sock, const char *id, const char *session_id = nullptr,
                          int proto_ver = PROTO_WIRE_VERSION)
    {
        std::string hello = std::string("{\"cmd\":\"device_hello\",\"id\":\"") + id +
                            "\",\"fw_version\":\"1.1.0\",\"proto_ver\":" +
                            std::to_string(proto_ver) + ",\"uptime\":10";
        hello += ",\"serial_profile\":{\"frame_size\":16,\"rows\":2,\"cols\":3,"
                 "\"data_points\":6,\"value_domain\":\"raw_adc\"}";
        if (session_id)
            hello += std::string(",\"session_id\":\"") + session_id + "\"";
        hello += "}";
        SendControl(sock, hello.c_str());
        server->Poll(now);
        std::string ack;
        RecvControl(sock, &ack, 1000);
        return ack;
    }
};

} // namespace

TEST(HostSession, FirstHelloAckOkFields)
{
    SessionFixture f;
    f.Init();
    void *sock = f.ConnectClient();
    ASSERT_NE(sock, nullptr);
    f.server->Poll(f.now); /* accept */
    std::string ack = f.Handshake(sock, "02:00:00:00:00:01");
    cJSON *json = cJSON_Parse(ack.c_str());
    ASSERT_NE(json, nullptr);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(json, "cmd")->valuestring, "host_ack");
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(json, "status")->valuestring, "ok");
    EXPECT_TRUE(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(json, "heartbeat_interval")));
    EXPECT_TRUE(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(json, "proto_ver")));
    EXPECT_TRUE(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json, "session_id")));
    /* beta v1.1：不再包含 server_time / fw_min_req */
    EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(json, "server_time"), nullptr);
    EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(json, "fw_min_req"), nullptr);
    cJSON_Delete(json);
    net_sock_close(f.dev_ctx, sock);
    f.Cleanup();
}

TEST(HostSession, HelloMissingRequiredFieldIsMalformed)
{
    SessionFixture f;
    f.Init();
    void *sock = f.ConnectClient();
    ASSERT_NE(sock, nullptr);
    f.server->Poll(f.now);
    /* 缺少 fw_version */
    f.SendControl(sock,
                  "{\"cmd\":\"device_hello\",\"id\":\"02:00:00:00:00:01\","
                  "\"proto_ver\":1,\"uptime\":10}");
    f.server->Poll(f.now);
    std::string ack;
    EXPECT_FALSE(f.RecvControl(sock, &ack, 300));
    EXPECT_EQ(f.registry.Size(), (size_t)0);
    net_sock_close(f.dev_ctx, sock);
    f.Cleanup();
}

TEST(HostSession, ProtoVersionUnsupported)
{
    SessionFixture f;
    f.Init();
    void *sock = f.ConnectClient();
    ASSERT_NE(sock, nullptr);
    f.server->Poll(f.now);
    std::string ack = f.Handshake(sock, "02:00:00:00:00:01", nullptr, 1);
    cJSON *json = cJSON_Parse(ack.c_str());
    ASSERT_NE(json, nullptr);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(json, "status")->valuestring, "fail");
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(json, "reason")->valuestring,
                 "unsupported_protocol");
    cJSON_Delete(json);
    net_sock_close(f.dev_ctx, sock);
    f.Cleanup();
}

TEST(HostSession, SecondConnectionBusyKeepsFirstOnline)
{
    SessionFixture f;
    f.Init();
    void *sock1 = f.ConnectClient();
    ASSERT_NE(sock1, nullptr);
    f.server->Poll(f.now);
    std::string ack1 = f.Handshake(sock1, "02:00:00:00:00:01");
    ASSERT_TRUE(ack1.find("\"ok\"") != std::string::npos);

    void *sock2 = f.ConnectClient();
    ASSERT_NE(sock2, nullptr);
    f.server->Poll(f.now); /* accept 阶段拒绝第二连接 */
    std::string ack2;
    ASSERT_TRUE(f.RecvControl(sock2, &ack2, 1000));
    cJSON *json = cJSON_Parse(ack2.c_str());
    ASSERT_NE(json, nullptr);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(json, "status")->valuestring, "busy");
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(json, "reason")->valuestring,
                 "single_device_only");
    cJSON_Delete(json);

    /* 第一设备仍在线 */
    DeviceEntry *first = f.registry.Find("02:00:00:00:00:01");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->state, "online");
    EXPECT_NE(first->conn, nullptr);
    EXPECT_TRUE(pc_events_was_seen("single_device_rejected"));

    net_sock_close(f.dev_ctx, sock1);
    net_sock_close(f.dev_ctx, sock2);
    f.Cleanup();
}

TEST(HostSession, SameIdOnlineReconnectRejected)
{
    SessionFixture f;
    f.Init();
    void *sock1 = f.ConnectClient();
    ASSERT_NE(sock1, nullptr);
    f.server->Poll(f.now);
    std::string ack1 = f.Handshake(sock1, "02:00:00:00:00:01");
    ASSERT_TRUE(ack1.find("\"ok\"") != std::string::npos);
    cJSON *j1 = cJSON_Parse(ack1.c_str());
    ASSERT_NE(j1, nullptr);
    std::string session1 = cJSON_GetObjectItemCaseSensitive(j1, "session_id")->valuestring;
    cJSON_Delete(j1);

    void *sock2 = f.ConnectClient();
    ASSERT_NE(sock2, nullptr);
    f.server->Poll(f.now);
    std::string ack2;
    ASSERT_TRUE(f.RecvControl(sock2, &ack2, 1000));
    EXPECT_TRUE(ack2.find("single_device_only") != std::string::npos);

    DeviceEntry *first = f.registry.Find("02:00:00:00:00:01");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->session_id, session1); /* 未被替换 */
    EXPECT_NE(first->conn, nullptr);

    net_sock_close(f.dev_ctx, sock1);
    net_sock_close(f.dev_ctx, sock2);
    f.Cleanup();
}

TEST(HostSession, OfflineResumeReusesSessionId)
{
    SessionFixture f;
    f.Init();
    void *sock1 = f.ConnectClient();
    ASSERT_NE(sock1, nullptr);
    f.server->Poll(f.now);
    std::string ack1 = f.Handshake(sock1, "02:00:00:00:00:01");
    cJSON *j1 = cJSON_Parse(ack1.c_str());
    ASSERT_NE(j1, nullptr);
    std::string session1 = cJSON_GetObjectItemCaseSensitive(j1, "session_id")->valuestring;
    cJSON_Delete(j1);

    net_sock_close(f.dev_ctx, sock1);
    f.server->Poll(f.now); /* 检测断线 → 离线 */
    DeviceEntry *e = f.registry.Find("02:00:00:00:00:01");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->state, "offline");

    void *sock2 = f.ConnectClient();
    ASSERT_NE(sock2, nullptr);
    f.server->Poll(f.now);
    std::string ack2 = f.Handshake(sock2, "02:00:00:00:00:01", session1.c_str());
    cJSON *j2 = cJSON_Parse(ack2.c_str());
    ASSERT_NE(j2, nullptr);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(j2, "session_id")->valuestring,
                 session1.c_str());
    cJSON_Delete(j2);
    DeviceEntry *e2 = f.registry.Find("02:00:00:00:00:01");
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(e2->reconnect_count, 1);

    net_sock_close(f.dev_ctx, sock2);
    f.Cleanup();
}

TEST(HostSession, PingEchoesSeq)
{
    SessionFixture f;
    f.Init();
    void *sock = f.ConnectClient();
    ASSERT_NE(sock, nullptr);
    f.server->Poll(f.now);
    f.Handshake(sock, "02:00:00:00:00:01");
    f.SendControl(sock, "{\"cmd\":\"ping\",\"seq\":42}");
    f.server->Poll(f.now);
    std::string pong;
    ASSERT_TRUE(f.RecvControl(sock, &pong, 1000));
    cJSON *json = cJSON_Parse(pong.c_str());
    ASSERT_NE(json, nullptr);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(json, "cmd")->valuestring, "pong");
    EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(json, "seq")->valueint, 42);
    cJSON_Delete(json);
    net_sock_close(f.dev_ctx, sock);
    f.Cleanup();
}

TEST(HostSession, HeartbeatTimeoutMarksOffline)
{
    SessionFixture f;
    f.Init();
    void *sock = f.ConnectClient();
    ASSERT_NE(sock, nullptr);
    f.server->Poll(f.now);
    f.Handshake(sock, "02:00:00:00:00:01");
    DeviceEntry *e = f.registry.Find("02:00:00:00:00:01");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->state, "online");

    /* 推进时间超过心跳判死阈值（heartbeat_dead_ms=0 → interval*3/2=15000ms） */
    f.now += 20000;
    f.server->Poll(f.now);
    EXPECT_EQ(f.registry.Find("02:00:00:00:00:01")->state, "offline");
    EXPECT_TRUE(pc_events_was_seen("session_offline"));
    net_sock_close(f.dev_ctx, sock);
    f.Cleanup();
}

TEST(HostSession, AppDataRxUtf8ByteCount)
{
    pc_events_close();
    const char *events_path = "run/test_session_appdata.jsonl";
    std::remove(events_path);
    ASSERT_EQ(pc_events_open(events_path), DEMO_OK);

    SessionFixture f;
    f.Init();
    void *sock = f.ConnectClient();
    ASSERT_NE(sock, nullptr);
    f.server->Poll(f.now);
    f.Handshake(sock, "02:00:00:00:00:01");
    /* "你好" 为 6 字节 UTF-8 */
    f.SendControl(sock,
                  "{\"cmd\":\"app_data\",\"seq\":1,\"data\":{\"type\":\"debug_text\","
                  "\"text\":\"\xe4\xbd\xa0\xe5\xa5\xbd\"}}");
    f.server->Poll(f.now);
    net_sock_close(f.dev_ctx, sock);
    f.Cleanup();
    pc_events_close();

    std::ifstream in(events_path);
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        cJSON *root = cJSON_Parse(line.c_str());
        ASSERT_NE(root, nullptr);
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        if (cJSON_IsString(event) && strcmp(event->valuestring, "app_data_rx") == 0) {
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
            const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(data, "text_bytes");
            EXPECT_TRUE(cJSON_IsNumber(bytes) && bytes->valueint == 6);
            found = true;
        }
        cJSON_Delete(root);
    }
    EXPECT_TRUE(found);
    std::remove(events_path);
    pc_events_close();
}
