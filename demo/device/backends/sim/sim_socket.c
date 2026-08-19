/* ============================================================================
 * sim_socket.c - TCP/UDP/组播与平台错误映射（纯 C11）。
 *
 * 迁移自旧 net_sim/sim_backend.cpp 的 socket/UDP 部分，剥离所有
 * Linux 真实热点/STA 分支；非阻塞 + 错误映射语义逐字保持。
 * ========================================================================== */
#include "sim_socket.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socket_length_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef socklen_t socket_length_t;
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

int sim_socket_startup(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? DEMO_OK : DEMO_ERR;
#else
    return DEMO_OK;
#endif
}

void sim_socket_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static void socket_set_nonblock(SOCKET s)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0)
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int socket_last_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static int socket_would_block(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

static int socket_connect_pending(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS;
#endif
}

static int socket_connection_refused(int error)
{
#ifdef _WIN32
    return error == WSAECONNREFUSED;
#else
    return error == ECONNREFUSED;
#endif
}

static void socket_close_impl(SOCKET s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static int socket_select_nfds(SOCKET s)
{
#ifdef _WIN32
    (void)s;
    return 0;
#else
    return (int)s + 1;
#endif
}

/* ---------------- TCP ---------------- */

int sim_socket_tcp_listen(uint16_t port, void **sock)
{
    if (!sock)
        return DEMO_ERR_INVAL;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return DEMO_ERR;
    socket_set_nonblock(s);
    /* 允许快速重启：上次运行的连接 socket 处于 TIME_WAIT 时不阻塞重新监听 */
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 模拟模式：仅监听 loopback */
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(s, 8) == SOCKET_ERROR) {
        socket_close_impl(s);
        return DEMO_ERR;
    }
    *sock = (void *)(intptr_t)s;
    return DEMO_OK;
}

int sim_socket_tcp_accept(void *listen_sock, void **conn, net_addr_t *peer)
{
    if (!conn)
        return DEMO_ERR_INVAL;
    SOCKET ls = (SOCKET)(intptr_t)listen_sock;
    struct sockaddr_in from;
    socket_length_t fromlen = sizeof(from);
    SOCKET c = accept(ls, (struct sockaddr *)&from, &fromlen);
    if (c == INVALID_SOCKET)
        return DEMO_ERR_AGAIN;
    socket_set_nonblock(c);
    if (peer) {
        peer->ip = from.sin_addr.s_addr;
        peer->port = from.sin_port;
    }
    *conn = (void *)(intptr_t)c;
    return DEMO_OK;
}

int sim_socket_tcp_connect(const net_addr_t *addr, void **sock, int timeout_ms)
{
    if (!addr || !sock)
        return DEMO_ERR_INVAL;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return DEMO_ERR;
    socket_set_nonblock(s);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = addr->ip;
    dst.sin_port = addr->port;
    int rc = connect(s, (struct sockaddr *)&dst, sizeof(dst));
    if (rc == SOCKET_ERROR) {
        int err = socket_last_error();
        if (!socket_connect_pending(err)) {
            socket_close_impl(s);
            /* 目标无监听（连接被拒）→ 视为"本次尝试无法建立"（可重试），与超时同类 */
            return socket_connection_refused(err) ? DEMO_ERR_TIMEOUT : DEMO_ERR;
        }
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv;
        int ms = timeout_ms <= 0 ? 3000 : timeout_ms;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        rc = select(socket_select_nfds(s), NULL, &wf, NULL, &tv);
        if (rc <= 0) {
            socket_close_impl(s);
            return DEMO_ERR_TIMEOUT;
        }
        int soerr = 0;
        socket_length_t soerrlen = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &soerrlen);
        if (soerr != 0) {
            socket_close_impl(s);
            /* 连接被拒（无监听）→ 与超时同类（可重试） */
            return socket_connection_refused(soerr) ? DEMO_ERR_TIMEOUT : DEMO_ERR;
        }
    }
    socket_set_nonblock(s);
    *sock = (void *)(intptr_t)s;
    return DEMO_OK;
}

int sim_socket_send(void *sock, const uint8_t *buf, int len)
{
    if (!sock || !buf || len < 0)
        return DEMO_ERR_INVAL;
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

int sim_socket_recv(void *sock, uint8_t *buf, int cap)
{
    if (!sock || !buf || cap <= 0)
        return DEMO_ERR_INVAL;
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

void sim_socket_close(void *sock)
{
    if (sock)
        socket_close_impl((SOCKET)(intptr_t)sock);
}

/* ---------------- UDP 组播 ---------------- */

int sim_socket_udp_mcast_join(const char *group, uint16_t port, void **sock)
{
    if (!group || !sock)
        return DEMO_ERR_INVAL;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return DEMO_ERR;
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&local, sizeof(local)) == SOCKET_ERROR) {
        socket_close_impl(s);
        return DEMO_ERR;
    }
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(group);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
        socket_close_impl(s);
        return DEMO_ERR;
    }
    socket_set_nonblock(s);
    *sock = (void *)(intptr_t)s;
    return DEMO_OK;
}

int sim_socket_udp_send(const char *group, uint16_t port, const uint8_t *buf, int len)
{
    if (!group || !buf || len < 0)
        return DEMO_ERR_INVAL;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return DEMO_ERR;
    char ttl = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = inet_addr(group);
    dst.sin_port = htons(port);
    int n = sendto(s, (const char *)buf, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    socket_close_impl(s);
    if (n == SOCKET_ERROR)
        return DEMO_ERR;
    return n;
}

int sim_socket_udp_recv(void *sock, uint8_t *buf, int cap, net_addr_t *from, int drain_until_block)
{
    if (!sock || !buf || cap <= 0)
        return DEMO_ERR_INVAL;
    SOCKET s = (SOCKET)(intptr_t)sock;
    struct sockaddr_in src;
    socket_length_t srclen = sizeof(src);
    if (drain_until_block) {
        /* 屏蔽时丢弃全部收包，直到 would-block */
        char tmp[2048];
        for (;;) {
            int n = recvfrom(s, tmp, sizeof(tmp), 0, (struct sockaddr *)&src, &srclen);
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
    int n = recvfrom(s, (char *)buf, cap, 0, (struct sockaddr *)&src, &srclen);
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
