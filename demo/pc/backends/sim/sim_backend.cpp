#include "sim_backend.h"
#include "sim_world.h"
#include "sim_tcp_endpoint.h"
#include "log.h"
#include "cJSON.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_length_t = int;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
using socket_length_t = socklen_t;
#endif

static void set_nonblock(SOCKET socket)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags >= 0)
        fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int socket_last_error()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool socket_would_block(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

static bool socket_connect_pending(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS;
#endif
}

static bool socket_connection_refused(int error)
{
#ifdef _WIN32
    return error == WSAECONNREFUSED;
#else
    return error == ECONNREFUSED;
#endif
}

static void socket_close(SOCKET socket)
{
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

static int socket_select_nfds(SOCKET socket)
{
#ifdef _WIN32
    (void)socket;
    return 0;
#else
    return socket + 1;
#endif
}

static void format_ipv4(uint32_t ip_network_order, char *buf, size_t cap)
{
    const uint8_t *b = (const uint8_t *)&ip_network_order;
    snprintf(buf, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

namespace {

/* ---------------- 工具 ---------------- */

int dev_index_of_tag(const char *tag)
{
    int idx = -1;
    if (sscanf(tag, "dev%d", &idx) != 1)
        return -1;
    return idx;
}

std::filesystem::path hotspot_catalog_path(const demo_params_t &params)
{
    if (params.sim_catalog_dir[0] != '\0')
        return std::filesystem::path(params.sim_catalog_dir) / "pc-hotspot.json";
    return std::filesystem::path("run") / "ap_catalog" / "pc-hotspot.json";
}

static int current_pid()
{
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

static uint64_t now_ms()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* ---- schema2 pc-hotspot.json 发布 ---- */

static long long hotspot_owner_pid(const std::filesystem::path &path)
{
    FILE *file = fopen(path.string().c_str(), "rb");
    if (!file)
        return -1;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 4096) {
        fclose(file);
        return -1;
    }
    std::string text((size_t)size, '\0');
    size_t read_size = fread(&text[0], 1, text.size(), file);
    fclose(file);
    text.resize(read_size);
    cJSON *root = cJSON_Parse(text.c_str());
    if (!root)
        return -1;
    const cJSON *owner = cJSON_GetObjectItemCaseSensitive(root, "owner_pid");
    long long pid = cJSON_IsNumber(owner) ? (long long)cJSON_GetNumberValue(owner) : -1;
    cJSON_Delete(root);
    return pid;
}

static int publish_hotspot_file(const demo_params_t &params, const char *ssid,
                                const char *password)
{
    std::filesystem::path target = hotspot_catalog_path(params);
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error)
        return DEMO_ERR;
    std::filesystem::path tmp = target;
    tmp += ".tmp-" + std::to_string(current_pid());

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return DEMO_ERR_NOMEM;
    cJSON_AddNumberToObject(root, "schema", 2);
    cJSON_AddStringToObject(root, "ssid", ssid ? ssid : "");
    cJSON_AddStringToObject(root, "password", password ? password : "");
    cJSON_AddStringToObject(root, "logical_gateway", PROTO_PC_AP_IP);
    cJSON_AddNumberToObject(root, "prefix_length", 24);
    cJSON_AddNumberToObject(root, "tcp_port", params.host_tcp_port);
    cJSON_AddStringToObject(root, "loopback_host", "127.0.0.1");
    cJSON_AddNumberToObject(root, "loopback_port", params.host_tcp_port);
    cJSON_AddNumberToObject(root, "owner_pid", current_pid());
    cJSON_AddNumberToObject(root, "published_at_ms", (double)now_ms());
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text)
        return DEMO_ERR_NOMEM;

    FILE *file = fopen(tmp.string().c_str(), "wb");
    if (!file) {
        free(text);
        return DEMO_ERR;
    }
    fwrite(text, 1, strlen(text), file);
    fclose(file);
    free(text);

    std::filesystem::rename(tmp, target, error);
    if (error) {
        std::filesystem::remove(tmp, error);
        return DEMO_ERR;
    }
    return DEMO_OK;
}

static void delete_hotspot_file_if_owned(const demo_params_t &params)
{
    std::filesystem::path target = hotspot_catalog_path(params);
    std::error_code error;
    if (!std::filesystem::exists(target, error))
        return;
    if (hotspot_owner_pid(target) == (long long)current_pid())
        std::filesystem::remove(target, error);
}

/* ---------------- SimBackend 实例 ---------------- */

class SimBackend {
public:
    SimBackend(const char *tag, const demo_params_t *params)
        : tag_(tag), params_(*params),
          rng_((uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() ^
               (uint32_t)strlen(tag) * 2654435761u)
    {
        if (dev_index_of_tag(tag) >= 0)
            rng_.seed(12345u + (uint32_t)dev_index_of_tag(tag) * 7919u);
        sta_connected_ = false;
        ap_started_ = false;
        ws_ready_ = EnsureWsa();
    }

    ~SimBackend()
    {
        CloseAllSockets();
    }

    bool initialized() const { return initialized_; }

    const std::string &tag() const { return tag_; }
    int dev_index() const { return dev_index_of_tag(tag_.c_str()); }

    /* ---- WiFi ---- */
    int WifiScan(net_ap_info_t *aps, int *count)
    {
        int cap = *count;
        int n = 0;
        SimWorld &w = SimWorld::Instance();
        int pos = 0;
        SimAp ap;
        while (w.ApIterate(&pos, &ap)) {
            if (n < cap) {
                memset(&aps[n], 0, sizeof(aps[n]));
                snprintf(aps[n].ssid, sizeof(aps[n].ssid), "%s", ap.ssid);
                aps[n].rssi = -50;
                aps[n].band_2g = 1;
            }
            n++;
        }
        if (w.TargetUp()) {
            if (n < cap) {
                memset(&aps[n], 0, sizeof(aps[n]));
                snprintf(aps[n].ssid, sizeof(aps[n].ssid), "%s", w.TargetSsid());
                aps[n].rssi = -45;
                aps[n].band_2g = w.TargetBand2g() ? 1 : 0;
            }
            n++;
        }
        *count = n;
        return DEMO_OK;
    }

    int WifiStaConnect(const char *ssid, const char *pass, wifi_reason_t *reason)
    {
        SimWorld &w = SimWorld::Instance();
        int r = w.StaConnect(ssid, pass);
        if (reason) *reason = (wifi_reason_t)r;
        if (r == 0) {
            sta_connected_ = true;
            sta_ssid_ = ssid;
            LOG_I(tag_.c_str(), "WiFi STA 已连接到 %s", ssid);
        } else {
            sta_connected_ = false;
            sta_ssid_.clear();
            LOG_I(tag_.c_str(), "WiFi STA 连接 %s 失败，原因码=%d", ssid, r);
        }
        return r == 0 ? DEMO_OK : DEMO_ERR;
    }

    int WifiStaDisconnect()
    {
        sta_connected_ = false;
        sta_ssid_.clear();
        LOG_I(tag_.c_str(), "WiFi STA 已断开");
        return DEMO_OK;
    }

    int WifiApStart(const char *ssid, const char *pass, const char *pin)
    {
        (void)pin; /* 设计文档 7.1：PIN 参数必须忽略 */
        if (!ssid || !ssid[0] || !pass || !pass[0])
            return DEMO_ERR_INVAL;
        int rc = publish_hotspot_file(params_, ssid, pass);
        if (rc != DEMO_OK)
            return rc;
        ap_started_ = true;
        ap_ssid_ = ssid;
        ap_ipv4_ = PROTO_PC_AP_IP;
        ap_prefix_ = 24;
        LOG_I(tag_.c_str(), "PC 热点已启动（模拟），SSID=%s，TCP=%d", ssid, params_.host_tcp_port);
        return DEMO_OK;
    }

    int WifiApStatus(net_ap_status_t *status)
    {
        if (!status)
            return DEMO_ERR_INVAL;
        memset(status, 0, sizeof(*status));
        if (!ap_started_)
            return DEMO_ERR;
        status->started = 1;
        snprintf(status->ssid, sizeof(status->ssid), "%s", ap_ssid_.c_str());
        snprintf(status->ipv4, sizeof(status->ipv4), "%s", ap_ipv4_.c_str());
        status->prefix_length = ap_prefix_;
        return DEMO_OK;
    }

    int WifiApConfigureIpv4(const char *ipv4, int prefix_length)
    {
        if (!ipv4 || !ipv4[0] || prefix_length <= 0 || prefix_length > 32)
            return DEMO_ERR_INVAL;
        if (!ap_started_)
            return DEMO_ERR;
        ap_ipv4_ = ipv4;
        ap_prefix_ = prefix_length;
        LOG_I(tag_.c_str(), "PC 热点 IP 已配置（模拟）：%s/%d", ipv4, prefix_length);
        return DEMO_OK;
    }

    int WifiApStop()
    {
        delete_hotspot_file_if_owned(params_);
        ap_started_ = false;
        ap_ssid_.clear();
        LOG_I(tag_.c_str(), "PC 热点已停止（模拟）");
        return DEMO_OK;
    }

    int WifiGetRssi(int *rssi)
    {
        SimWorld &w = SimWorld::Instance();
        *rssi = w.RssiGet(tag_.c_str());
        return DEMO_OK;
    }

    int WifiGetIp(uint32_t *ip)
    {
        SimWorld &w = SimWorld::Instance();
        if (sta_connected_ && sta_ssid_ == w.TargetSsid() && !w.TargetUp()) {
            *ip = 0;
            return DEMO_ERR;
        }
        int idx = dev_index();
        if (idx >= 0) {
            if (sta_connected_)
                *ip = SimWorld::DeviceStaVirtualIp(idx);
            else if (ap_started_)
                *ip = SimWorld::DeviceApVirtualIp();
            else
                *ip = 0;
        } else {
            *ip = SimWorld::HostVirtualIp();
        }
        return DEMO_OK;
    }

    int WifiGetCurrentSsid(char *ssid, int capacity)
    {
        if (!ssid || capacity <= 0 || !sta_connected_ || sta_ssid_.empty())
            return DEMO_ERR;
        SimWorld &w = SimWorld::Instance();
        if (sta_ssid_ == w.TargetSsid() && !w.TargetUp())
            return DEMO_ERR;
        snprintf(ssid, (size_t)capacity, "%s", sta_ssid_.c_str());
        return DEMO_OK;
    }

    int WifiGetGateway(uint32_t *ip)
    {
        if (!ip || !sta_connected_)
            return DEMO_ERR;
        *ip = SimWorld::DeviceApVirtualIp();
        return DEMO_OK;
    }

    /* ---- TCP ---- */
    int TcpListen(uint16_t port, void **sock)
    {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return DEMO_ERR;
        set_nonblock(s);
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(s, (sockaddr *)&addr, sizeof(addr)) == -1 ||
            listen(s, 8) == -1) {
            socket_close(s);
            return DEMO_ERR;
        }
        *sock = (void *)s;
        LOG_I(tag_.c_str(), "TCP 正在监听 127.0.0.1:%u", (unsigned)port);
        return DEMO_OK;
    }

    int TcpAccept(void *listen, void **conn, net_addr_t *peer)
    {
        SOCKET ls = (SOCKET)(intptr_t)listen;
        sockaddr_in from;
        socket_length_t fromlen = sizeof(from);
        SOCKET c = accept(ls, (sockaddr *)&from, &fromlen);
        if (c == INVALID_SOCKET)
            return DEMO_ERR_AGAIN;
        set_nonblock(c);
        if (peer) {
            peer->ip = from.sin_addr.s_addr;
            peer->port = from.sin_port;
        }
        *conn = (void *)c;
        return DEMO_OK;
    }

    int TcpConnect(const net_addr_t *addr, void **sock, int timeout_ms)
    {
        if (addr == nullptr)
            return DEMO_ERR_INVAL;

        SimWorld &w = SimWorld::Instance();
        uint32_t vaddr = addr->ip;
        bool ap_match = false;
        uint16_t ap_real_port_host = 0;
        if (vaddr == SimWorld::DeviceApVirtualIp()) {
            int pos = 0;
            SimAp ap;
            if (w.ApIterate(&pos, &ap) && ap.active) {
                ap_match = true;
                ap_real_port_host = ap.real_port;
            }
        }
        sim_tcp_endpoint_t ep = sim_tcp_resolve_endpoint(
            0, addr, ap_match ? 1 : 0, ap_real_port_host,
            SimWorld::HostVirtualIp(), (uint16_t)params_.host_tcp_port);

        char req_ip[16], eff_ip[16];
        format_ipv4(addr->ip, req_ip, sizeof(req_ip));
        format_ipv4(ep.ip, eff_ip, sizeof(eff_ip));
        LOG_I(tag_.c_str(), "TCP连接：模拟翻译，请求=%s:%u，实际=%s:%u",
              req_ip, (unsigned)ntohs(addr->port),
              eff_ip, (unsigned)ntohs(ep.port));

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            LOG_W(tag_.c_str(), "TCP连接失败：阶段=create，实际=%s:%u，错误码=%d",
                  eff_ip, (unsigned)ntohs(ep.port), socket_last_error());
            return DEMO_ERR;
        }
        set_nonblock(s);
        sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = ep.ip;
        dst.sin_port = ep.port;
        int rc = connect(s, (sockaddr *)&dst, sizeof(dst));
        if (rc == SOCKET_ERROR) {
            int err = socket_last_error();
            if (!socket_connect_pending(err)) {
                socket_close(s);
                LOG_W(tag_.c_str(), "TCP连接失败：阶段=immediate，实际=%s:%u，错误码=%d",
                      eff_ip, (unsigned)ntohs(ep.port), err);
                return socket_connection_refused(err) ? DEMO_ERR_TIMEOUT : DEMO_ERR;
            }
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv;
            int ms = timeout_ms <= 0 ? 3000 : timeout_ms;
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            rc = select(socket_select_nfds(s), nullptr, &wf, nullptr, &tv);
            if (rc <= 0) {
                socket_close(s);
                LOG_W(tag_.c_str(), "TCP连接失败：阶段=wait，实际=%s:%u，错误码=%d",
                      eff_ip, (unsigned)ntohs(ep.port), rc);
                return DEMO_ERR_TIMEOUT;
            }
            int soerr = 0;
            socket_length_t soerrlen = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &soerrlen);
            if (soerr != 0) {
                socket_close(s);
                LOG_W(tag_.c_str(), "TCP连接失败：阶段=so_error，实际=%s:%u，错误码=%d",
                      eff_ip, (unsigned)ntohs(ep.port), soerr);
                return socket_connection_refused(soerr) ? DEMO_ERR_TIMEOUT : DEMO_ERR;
            }
        }
        set_nonblock(s);
        *sock = (void *)s;
        LOG_I(tag_.c_str(), "TCP连接成功：实际=%s:%u",
              eff_ip, (unsigned)ntohs(ep.port));
        return DEMO_OK;
    }

    int SockSend(void *sock, const uint8_t *buf, int len)
    {
        if (send_fail_skip_ > 0) {
            --send_fail_skip_;
        } else if (send_fail_count_ > 0) {
            --send_fail_count_;
            LOG_I(tag_.c_str(), "故障注入：SockSend 被强制失败（剩余 %d 次）",
                  send_fail_count_);
            return DEMO_ERR;
        }
        SOCKET s = (SOCKET)(intptr_t)sock;
        int n = send(s, (const char *)buf, len, 0);
        if (n == SOCKET_ERROR) {
            int err = socket_last_error();
            if (socket_would_block(err))
                return DEMO_ERR_AGAIN;
            return DEMO_ERR;
        }
        if (n == 0)
            return DEMO_ERR;
        return n;
    }

    int SockRecv(void *sock, uint8_t *buf, int cap)
    {
        SOCKET s = (SOCKET)(intptr_t)sock;
        int n = recv(s, (char *)buf, cap, 0);
        if (n == SOCKET_ERROR) {
            int err = socket_last_error();
            if (socket_would_block(err))
                return DEMO_ERR_AGAIN;
            return DEMO_ERR;
        }
        if (n == 0)
            return DEMO_ERR;
        return n;
    }

    void SockClose(void *sock)
    {
        if (sock)
            socket_close((SOCKET)(intptr_t)sock);
    }

    /* ---- UDP 组播 ---- */
    int UdpMcastJoin(const char *group, uint16_t port, void **sock)
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return DEMO_ERR;
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
        sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(port);
        if (bind(s, (sockaddr *)&local, sizeof(local)) == -1) {
            socket_close(s);
            return DEMO_ERR;
        }
        ip_mreq mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.imr_multiaddr.s_addr = inet_addr(group);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) == -1) {
            socket_close(s);
            return DEMO_ERR;
        }
        set_nonblock(s);
        *sock = (void *)s;
        LOG_I(tag_.c_str(), "已加入组播 %s:%u", group, (unsigned)port);
        return DEMO_OK;
    }

    int UdpSend(const char *group, uint16_t port, const uint8_t *buf, int len)
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return DEMO_ERR;
        char ttl = 1;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = inet_addr(group);
        dst.sin_port = htons(port);
        int n = sendto(s, (const char *)buf, len, 0, (sockaddr *)&dst, sizeof(dst));
        socket_close(s);
        if (n == SOCKET_ERROR)
            return DEMO_ERR;
        return n;
    }

    int UdpRecv(void *sock, uint8_t *buf, int cap, net_addr_t *from)
    {
        SOCKET s = (SOCKET)(intptr_t)sock;
        SimWorld &w = SimWorld::Instance();
        if (w.McastIsBlocked(tag_.c_str())) {
            char tmp[2048];
            sockaddr_in src;
            socket_length_t srclen = sizeof(src);
            while (true) {
                int n = recvfrom(s, tmp, sizeof(tmp), 0, (sockaddr *)&src, &srclen);
                if (n == SOCKET_ERROR) {
                    int err = socket_last_error();
                    if (socket_would_block(err))
                        return DEMO_ERR_AGAIN;
                    return DEMO_ERR;
                }
                if (n == 0)
                    return DEMO_ERR;
            }
        }
        sockaddr_in src;
        socket_length_t srclen = sizeof(src);
        int n = recvfrom(s, (char *)buf, cap, 0, (sockaddr *)&src, &srclen);
        if (n == SOCKET_ERROR) {
            int err = socket_last_error();
            if (socket_would_block(err))
                return DEMO_ERR_AGAIN;
            return DEMO_ERR;
        }
        if (n == 0)
            return DEMO_ERR;
        if (from) {
            from->ip = src.sin_addr.s_addr;
            from->port = src.sin_port;
        }
        return n;
    }

    /* ---- mDNS（进程内模拟注册表） ---- */
    int MdnsRegister(const net_mdns_service_t *svc)
    {
        SimMdnsSvc s;
        memset(&s, 0, sizeof(s));
        snprintf(s.instance, sizeof(s.instance), "%s", svc->instance);
        snprintf(s.type, sizeof(s.type), "%s", svc->type);
        s.ip = svc->addr.ip;
        s.port = svc->addr.port;
        snprintf(s.txt, sizeof(s.txt), "%s", svc->txt);
        s.active = true;
        SimWorld::Instance().MdnsRegister(s);
        LOG_I(tag_.c_str(), "mDNS 服务已注册：%s", svc->type);
        return DEMO_OK;
    }

    int MdnsUnregister(const char *type)
    {
        SimWorld::Instance().MdnsUnregister(type);
        return DEMO_OK;
    }

    int MdnsResolve(const char *type, net_mdns_service_t *out, int timeout_ms)
    {
        SimWorld &w = SimWorld::Instance();
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms <= 0 ? 2000 : timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            SimMdnsSvc s;
            if (w.MdnsResolve(type, &s)) {
                memset(out, 0, sizeof(*out));
                snprintf(out->instance, sizeof(out->instance), "%s", s.instance);
                snprintf(out->type, sizeof(out->type), "%s", s.type);
                out->addr.ip = s.ip;
                out->addr.port = s.port;
                snprintf(out->txt, sizeof(out->txt), "%s", s.txt);
                return DEMO_OK;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return DEMO_ERR_TIMEOUT;
    }

    /* ---- 系统 ---- */
    uint64_t TimeMs()
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint32_t Random() { return rng_(); }

    /* ---- 注入 ---- */
    int Inject(const char *action, const char *arg_json)
    {
        SimWorld &w = SimWorld::Instance();
        if (strcmp(action, "wifi_disconnect") == 0) {
            w.TargetSetUp(false);
            LOG_I(tag_.c_str(), "故障注入：目标 WiFi 已关闭");
            return DEMO_OK;
        }
        if (strcmp(action, "wifi_ok") == 0) {
            w.TargetSetUp(true);
            LOG_I(tag_.c_str(), "故障注入：目标 WiFi 已恢复");
            return DEMO_OK;
        }
        if (strcmp(action, "wifi_auth_fail") == 0) {
            w.TargetSetAuthFail(true);
            LOG_I(tag_.c_str(), "故障注入：目标 WiFi 强制认证失败");
            return DEMO_OK;
        }
        if (strcmp(action, "wifi_auth_ok") == 0) {
            w.TargetSetAuthFail(false);
            return DEMO_OK;
        }
        if (strcmp(action, "wifi_ssid_mismatch") == 0) {
            if (!sta_connected_)
                return DEMO_ERR;
            sta_ssid_ = (arg_json && arg_json[0]) ? arg_json : "UnexpectedWifi";
            LOG_I(tag_.c_str(), "故障注入：当前关联 SSID 已改为 %s", sta_ssid_.c_str());
            return DEMO_OK;
        }
        if (strcmp(action, "rssi_set") == 0) {
            int rssi = -80;
            if (arg_json) {
                int v = 0;
                if (sscanf(arg_json, "%d", &v) == 1)
                    rssi = v;
                else {
                    const char *p = strstr(arg_json, "\"rssi\"");
                    if (p) sscanf(p, "\"rssi\":%d", &rssi);
                }
            }
            w.RssiSet(tag_.c_str(), rssi);
            LOG_I(tag_.c_str(), "故障注入：RSSI 已设置为 %d", rssi);
            return DEMO_OK;
        }
        if (strcmp(action, "mcast_block") == 0) {
            w.McastSetBlocked(tag_.c_str(), true);
            return DEMO_OK;
        }
        if (strcmp(action, "mcast_unblock") == 0) {
            w.McastSetBlocked(tag_.c_str(), false);
            return DEMO_OK;
        }
        if (strcmp(action, "burst_send") == 0) {
            LOG_I(tag_.c_str(), "故障注入：已请求突发发送");
            return DEMO_OK;
        }
        if (strcmp(action, "sock_send_fail") == 0) {
            int skip = 0, count = 1;
            cJSON *arg = (arg_json && arg_json[0]) ? cJSON_Parse(arg_json) : nullptr;
            if (arg) {
                const cJSON *s = cJSON_GetObjectItemCaseSensitive(arg, "skip");
                const cJSON *c = cJSON_GetObjectItemCaseSensitive(arg, "count");
                if (cJSON_IsNumber(s)) skip = s->valueint;
                if (cJSON_IsNumber(c)) count = c->valueint;
                cJSON_Delete(arg);
            }
            if (skip < 0) skip = 0;
            if (count < 0) count = 0;
            send_fail_skip_ = skip;
            send_fail_count_ = count;
            LOG_I(tag_.c_str(), "故障注入：SockSend 将在跳过 %d 次成功后失败 %d 次",
                  skip, count);
            return DEMO_OK;
        }
        return DEMO_ERR;
    }

    void CloseAllSockets()
    {
        if (ap_started_) {
            delete_hotspot_file_if_owned(params_);
            ap_started_ = false;
        }
    }

private:
    static bool EnsureWsa()
    {
#ifdef _WIN32
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        return true;
#endif
    }

    std::string tag_;
    demo_params_t params_;
    std::mt19937 rng_;
    bool sta_connected_ = false;
    std::string sta_ssid_;
    bool ap_started_ = false;
    std::string ap_ssid_;
    std::string ap_ipv4_ = PROTO_PC_AP_IP;
    int ap_prefix_ = 24;
    bool ws_ready_ = false;
    bool initialized_ = true;
    int send_fail_skip_ = 0;
    int send_fail_count_ = 0;
};

/* ---------------- vtable 适配 ---------------- */

SimBackend *self(void *user) { return static_cast<SimBackend *>(user); }

int b_wifi_scan(void *u, net_ap_info_t *aps, int *count) { return self(u)->WifiScan(aps, count); }
int b_wifi_sta_connect(void *u, const char *s, const char *p, wifi_reason_t *r) { return self(u)->WifiStaConnect(s, p, r); }
int b_wifi_sta_disconnect(void *u) { return self(u)->WifiStaDisconnect(); }
int b_wifi_ap_start(void *u, const char *s, const char *p, const char *pin) { return self(u)->WifiApStart(s, p, pin); }
int b_wifi_ap_stop(void *u) { return self(u)->WifiApStop(); }
int b_wifi_ap_status(void *u, net_ap_status_t *s) { return self(u)->WifiApStatus(s); }
int b_wifi_ap_configure_ipv4(void *u, const char *ip, int pl) { return self(u)->WifiApConfigureIpv4(ip, pl); }
int b_wifi_get_rssi(void *u, int *r) { return self(u)->WifiGetRssi(r); }
int b_wifi_get_ip(void *u, uint32_t *ip) { return self(u)->WifiGetIp(ip); }
int b_wifi_get_current_ssid(void *u, char *ssid, int capacity) { return self(u)->WifiGetCurrentSsid(ssid, capacity); }
int b_wifi_get_gateway(void *u, uint32_t *ip) { return self(u)->WifiGetGateway(ip); }
int b_tcp_listen(void *u, uint16_t p, void **s) { return self(u)->TcpListen(p, s); }
int b_tcp_accept(void *u, void *l, void **c, net_addr_t *peer) { return self(u)->TcpAccept(l, c, peer); }
int b_tcp_connect(void *u, const net_addr_t *a, void **s, int t) { return self(u)->TcpConnect(a, s, t); }
int b_sock_send(void *u, void *s, const uint8_t *b, int l) { return self(u)->SockSend(s, b, l); }
int b_sock_recv(void *u, void *s, uint8_t *b, int c) { return self(u)->SockRecv(s, b, c); }
void b_sock_close(void *u, void *s) { self(u)->SockClose(s); }
int b_udp_mcast_join(void *u, const char *g, uint16_t p, void **s) { return self(u)->UdpMcastJoin(g, p, s); }
int b_udp_send(void *u, const char *g, uint16_t p, const uint8_t *b, int l) { return self(u)->UdpSend(g, p, b, l); }
int b_udp_recv(void *u, void *s, uint8_t *b, int c, net_addr_t *f) { return self(u)->UdpRecv(s, b, c, f); }
int b_mdns_register(void *u, const net_mdns_service_t *s) { return self(u)->MdnsRegister(s); }
int b_mdns_unregister(void *u, const char *t) { return self(u)->MdnsUnregister(t); }
int b_mdns_resolve(void *u, const char *t, net_mdns_service_t *o, int ms) { return self(u)->MdnsResolve(t, o, ms); }
uint64_t b_time_ms(void *u) { return self(u)->TimeMs(); }
uint32_t b_random(void *u) { return self(u)->Random(); }
int b_inject(void *u, const char *a, const char *j) { return self(u)->Inject(a, j); }

const net_backend_t g_sim_backend = {
    nullptr, nullptr,
    b_wifi_scan, b_wifi_sta_connect, b_wifi_sta_disconnect,
    b_wifi_ap_start, b_wifi_ap_stop, b_wifi_ap_status, b_wifi_ap_configure_ipv4,
    b_wifi_get_rssi, b_wifi_get_ip,
    b_wifi_get_current_ssid, b_wifi_get_gateway,
    b_tcp_listen, b_tcp_accept, b_tcp_connect,
    b_sock_send, b_sock_recv, b_sock_close,
    b_udp_mcast_join, b_udp_send, b_udp_recv,
    b_mdns_register, b_mdns_unregister, b_mdns_resolve,
    nullptr, nullptr, nullptr,          /* nvs：PC sim 不含设备 NVS */
    b_time_ms, b_random,
    b_inject,
};

} // namespace

extern "C" {

void *sim_backend_create(const char *tag, const demo_params_t *params)
{
    if (tag == nullptr || params == nullptr)
        return nullptr;
    SimBackend *backend = new SimBackend(tag, params);
    if (!backend->initialized()) {
        delete backend;
        return nullptr;
    }
    return backend;
}

void sim_backend_destroy(void *user)
{
    delete static_cast<SimBackend *>(user);
}

const net_backend_t *sim_backend_table(void)
{
    return &g_sim_backend;
}

} // extern "C"
