#include "pc_socket_backend.h"
#include "log.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_length_t = int;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
using socket_length_t = socklen_t;
#endif

namespace {

struct SocketBackend {
    bool ws_ready = false;
    std::mt19937 rng{(uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()};
};

void set_nonblock(SOCKET socket)
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

int socket_last_error()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool socket_would_block(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

bool socket_connect_pending(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS;
#endif
}

bool socket_connection_refused(int error)
{
#ifdef _WIN32
    return error == WSAECONNREFUSED;
#else
    return error == ECONNREFUSED;
#endif
}

void socket_close(SOCKET socket)
{
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

int socket_select_nfds(SOCKET socket)
{
#ifdef _WIN32
    (void)socket;
    return 0;
#else
    return socket + 1;
#endif
}

void format_ipv4(uint32_t ip_network_order, char *buf, size_t cap)
{
    const uint8_t *b = (const uint8_t *)&ip_network_order;
    snprintf(buf, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

bool ensure_wsa()
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

/* ---- TCP ---- */
int b_tcp_listen(void *user, uint16_t port, void **sock)
{
    (void)user;
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
    if (bind(s, (sockaddr *)&addr, sizeof(addr)) == -1 || listen(s, 8) == -1) {
        socket_close(s);
        return DEMO_ERR;
    }
    *sock = (void *)s;
    LOG_I("SOCK", "TCP 正在监听 127.0.0.1:%u", (unsigned)port);
    return DEMO_OK;
}

int b_tcp_accept(void *user, void *listen, void **conn, net_addr_t *peer)
{
    (void)user;
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

int b_tcp_connect(void *user, const net_addr_t *addr, void **sock, int timeout_ms)
{
    (void)user;
    if (addr == nullptr)
        return DEMO_ERR_INVAL;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return DEMO_ERR;
    set_nonblock(s);
    sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = addr->ip;
    dst.sin_port = addr->port;
    int rc = connect(s, (sockaddr *)&dst, sizeof(dst));
    if (rc == SOCKET_ERROR) {
        int err = socket_last_error();
        if (!socket_connect_pending(err)) {
            socket_close(s);
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
            return DEMO_ERR_TIMEOUT;
        }
        int soerr = 0;
        socket_length_t soerrlen = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &soerrlen);
        if (soerr != 0) {
            socket_close(s);
            return socket_connection_refused(soerr) ? DEMO_ERR_TIMEOUT : DEMO_ERR;
        }
    }
    set_nonblock(s);
    *sock = (void *)s;
    return DEMO_OK;
}

int b_sock_send(void *user, void *sock, const uint8_t *buf, int len)
{
    (void)user;
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

int b_sock_recv(void *user, void *sock, uint8_t *buf, int cap)
{
    (void)user;
    SOCKET s = (SOCKET)(intptr_t)sock;
    int n = recv(s, (char *)buf, cap, 0);
    if (n == SOCKET_ERROR) {
        int err = socket_last_error();
        if (socket_would_block(err))
            return DEMO_ERR_AGAIN;
        return DEMO_ERR;
    }
    if (n == 0)
        return DEMO_ERR; /* 对端关闭 */
    return n;
}

void b_sock_close(void *user, void *sock)
{
    (void)user;
    if (sock)
        socket_close((SOCKET)(intptr_t)sock);
}

/* ---- UDP 组播 ---- */
int b_udp_mcast_join(void *user, const char *group, uint16_t port, void **sock)
{
    (void)user;
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
    LOG_I("SOCK", "已加入组播 %s:%u", group, (unsigned)port);
    return DEMO_OK;
}

int b_udp_send(void *user, const char *group, uint16_t port, const uint8_t *buf, int len)
{
    (void)user;
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

int b_udp_recv(void *user, void *sock, uint8_t *buf, int cap, net_addr_t *from)
{
    (void)user;
    SOCKET s = (SOCKET)(intptr_t)sock;
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

/* ---- 系统 ---- */
uint64_t b_time_ms(void *)
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint32_t b_random(void *user)
{
    return static_cast<SocketBackend *>(user)->rng();
}

const net_backend_t g_socket_backend = {
    nullptr, nullptr,                        /* init / deinit */
    nullptr, nullptr, nullptr,               /* wifi */
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    b_tcp_listen, b_tcp_accept, b_tcp_connect,
    b_sock_send, b_sock_recv, b_sock_close,
    b_udp_mcast_join, b_udp_send, b_udp_recv,
    nullptr, nullptr, nullptr,               /* mdns */
    nullptr, nullptr, nullptr,               /* nvs */
    b_time_ms, b_random,
    nullptr,                                 /* inject */
};

} // namespace

extern "C" {

void *pc_socket_backend_create(void)
{
    SocketBackend *backend = new SocketBackend();
    backend->ws_ready = ensure_wsa();
    return backend;
}

void pc_socket_backend_destroy(void *user)
{
    delete static_cast<SocketBackend *>(user);
}

const net_backend_t *pc_socket_backend_table(void)
{
    return &g_socket_backend;
}

}
