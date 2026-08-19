#include "linux_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        return -1;
    return 0;
}

static int fd_of(void *sock)
{
    return (int)(intptr_t)sock;
}

int linux_socket_tcp_listen(uint16_t port, void **out_sock)
{
    int fd;
    int one = 1;
    struct sockaddr_in address;

    if (out_sock == NULL)
        return DEMO_ERR_INVAL;
    *out_sock = NULL;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return DEMO_ERR;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 4) != 0 || set_nonblocking(fd) != 0) {
        close(fd);
        return DEMO_ERR;
    }
    *out_sock = (void *)(intptr_t)fd;
    return DEMO_OK;
}

int linux_socket_tcp_accept(void *listen_sock, void **out_conn,
                            net_addr_t *out_peer)
{
    int fd;
    int conn;
    struct sockaddr_in peer;
    socklen_t peer_length = sizeof(peer);

    if (listen_sock == NULL || out_conn == NULL)
        return DEMO_ERR_INVAL;
    *out_conn = NULL;
    fd = fd_of(listen_sock);
    conn = accept4(fd, (struct sockaddr *)&peer, &peer_length, SOCK_CLOEXEC);
    if (conn < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return DEMO_ERR_AGAIN;
        return DEMO_ERR;
    }
    if (set_nonblocking(conn) != 0) {
        close(conn);
        return DEMO_ERR;
    }
    if (out_peer != NULL) {
        out_peer->ip = peer.sin_addr.s_addr;
        out_peer->port = peer.sin_port;
    }
    *out_conn = (void *)(intptr_t)conn;
    return DEMO_OK;
}

int linux_socket_tcp_connect(const net_addr_t *addr, void **out_sock,
                             int timeout_ms)
{
    int fd;
    struct sockaddr_in address;
    int result;
    int error;
    socklen_t error_length = sizeof(error);

    if (addr == NULL || out_sock == NULL)
        return DEMO_ERR_INVAL;
    *out_sock = NULL;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return DEMO_ERR;
    if (set_nonblocking(fd) != 0) {
        close(fd);
        return DEMO_ERR;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = addr->ip;
    address.sin_port = addr->port;
    result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result != 0 && errno == EINPROGRESS) {
        struct timeval timeout;
        fd_set write_fds;
        int select_result;

        if (timeout_ms > 0) {
            timeout.tv_sec = timeout_ms / 1000;
            timeout.tv_usec = (timeout_ms % 1000) * 1000;
        }
        FD_ZERO(&write_fds);
        FD_SET(fd, &write_fds);
        select_result = select(fd + 1, NULL, &write_fds, NULL,
                               timeout_ms > 0 ? &timeout : NULL);
        if (select_result <= 0) {
            close(fd);
            return select_result == 0 ? DEMO_ERR_TIMEOUT : DEMO_ERR;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 ||
            error != 0) {
            close(fd);
            return DEMO_ERR;
        }
        result = 0;
    }
    if (result != 0) {
        close(fd);
        return DEMO_ERR;
    }
    *out_sock = (void *)(intptr_t)fd;
    return DEMO_OK;
}

int linux_socket_send(void *sock, const uint8_t *buf, int len)
{
    ssize_t sent;
    int fd;

    if (sock == NULL || buf == NULL || len < 0)
        return DEMO_ERR_INVAL;
    fd = fd_of(sock);
    sent = send(fd, buf, (size_t)len, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return DEMO_ERR_AGAIN;
        return DEMO_ERR;
    }
    if (sent == 0)
        return DEMO_ERR;
    return (int)sent;
}

int linux_socket_recv(void *sock, uint8_t *buf, int cap)
{
    ssize_t received;
    int fd;

    if (sock == NULL || buf == NULL || cap <= 0)
        return DEMO_ERR_INVAL;
    fd = fd_of(sock);
    received = recv(fd, buf, (size_t)cap, 0);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return DEMO_ERR_AGAIN;
        return DEMO_ERR;
    }
    if (received == 0)
        return DEMO_ERR;
    return (int)received;
}

void linux_socket_close(void *sock)
{
    if (sock == NULL)
        return;
    close(fd_of(sock));
}

int linux_socket_udp_mcast_join(const char *group, uint16_t port, void **out_sock)
{
    int fd;
    int one = 1;
    struct sockaddr_in address;
    struct ip_mreq membership;

    if (group == NULL || out_sock == NULL)
        return DEMO_ERR_INVAL;
    *out_sock = NULL;
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return DEMO_ERR;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&membership, 0, sizeof(membership));
    membership.imr_multiaddr.s_addr = inet_addr(group);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                   sizeof(membership)) != 0) {
        close(fd);
        return DEMO_ERR;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        set_nonblocking(fd) != 0) {
        close(fd);
        return DEMO_ERR;
    }
    *out_sock = (void *)(intptr_t)fd;
    return DEMO_OK;
}

int linux_socket_udp_send(const char *group, uint16_t port, const uint8_t *buf,
                          int len)
{
    int fd;
    int ttl = 1;
    struct sockaddr_in destination;
    ssize_t sent;

    if (group == NULL || buf == NULL || len < 0)
        return DEMO_ERR_INVAL;
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return DEMO_ERR;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = inet_addr(group);
    destination.sin_port = htons(port);
    sent = sendto(fd, buf, (size_t)len, 0, (struct sockaddr *)&destination,
                  sizeof(destination));
    close(fd);
    return sent == (ssize_t)len ? DEMO_OK : DEMO_ERR;
}

int linux_socket_udp_recv(void *sock, uint8_t *buf, int cap, net_addr_t *out_from)
{
    struct sockaddr_in from;
    socklen_t from_length = sizeof(from);
    ssize_t received;
    int fd;

    if (sock == NULL || buf == NULL || cap <= 0)
        return DEMO_ERR_INVAL;
    fd = fd_of(sock);
    received = recvfrom(fd, buf, (size_t)cap, 0, (struct sockaddr *)&from,
                        &from_length);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return DEMO_ERR_AGAIN;
        return DEMO_ERR;
    }
    if (out_from != NULL) {
        out_from->ip = from.sin_addr.s_addr;
        out_from->port = from.sin_port;
    }
    return (int)received;
}

uint64_t linux_socket_time_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

uint32_t linux_socket_random(void)
{
    uint32_t value = 0;
    FILE *file = fopen("/dev/urandom", "rb");
    if (file != NULL) {
        if (fread(&value, sizeof(value), 1, file) == 1)
            value = (uint32_t)time(NULL) ^ value;
        fclose(file);
        return value;
    }
    return (uint32_t)(time(NULL) ^ getpid());
}
