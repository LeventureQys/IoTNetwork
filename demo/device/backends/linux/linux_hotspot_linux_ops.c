#include "linux_hotspot_linux_ops.h"

#include "linux_exec.h"

#ifdef __linux__

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int default_run_argv(const char *const argv[])
{
    return linux_exec_argv(argv, NULL, 0);
}

static int interface_is_up(unsigned int ifindex)
{
    char name[IF_NAMESIZE];
    char path[128];
    FILE *file;
    unsigned int flags;

    if (if_indextoname(ifindex, name) == NULL)
        return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/flags", name);
    file = fopen(path, "r");
    if (file == NULL)
        return 0;
    if (fscanf(file, "%x", &flags) != 1) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return (flags & IFF_UP) != 0;
}

static int append_route(const struct rtmsg *message, struct rtattr *attribute,
                        int attribute_length, linux_ipv4_route_t *routes,
                        size_t capacity, size_t *count)
{
    uint32_t destination = 0;
    unsigned int ifindex = 0;
    unsigned int table = message->rtm_table;
    int has_gateway = 0;
    linux_ipv4_route_t *route;

    for (; RTA_OK(attribute, attribute_length);
         attribute = RTA_NEXT(attribute, attribute_length)) {
        switch (attribute->rta_type) {
        case RTA_DST:
            if (RTA_PAYLOAD(attribute) < sizeof(destination))
                return -1;
            memcpy(&destination, RTA_DATA(attribute), sizeof(destination));
            break;
        case RTA_OIF:
            if (RTA_PAYLOAD(attribute) < sizeof(ifindex))
                return -1;
            memcpy(&ifindex, RTA_DATA(attribute), sizeof(ifindex));
            break;
        case RTA_GATEWAY:
            has_gateway = 1;
            break;
        case RTA_TABLE:
            if (RTA_PAYLOAD(attribute) < sizeof(table))
                return -1;
            memcpy(&table, RTA_DATA(attribute), sizeof(table));
            break;
        default:
            break;
        }
    }
    if (attribute_length != 0)
        return -1;
    if (ifindex == 0 || has_gateway || !interface_is_up(ifindex))
        return 0;
    if (*count >= capacity)
        return -1;
    route = &routes[(*count)++];
    memset(route, 0, sizeof(*route));
    route->destination.address = ntohl(destination);
    route->destination.prefix_length = message->rtm_dst_len;
    route->destination.netmask = message->rtm_dst_len == 0
                                     ? 0U
                                     : UINT32_MAX << (32 - message->rtm_dst_len);
    route->destination.network = route->destination.address & route->destination.netmask;
    route->destination.broadcast = route->destination.network |
                                   ~route->destination.netmask;
    route->ifindex = ifindex;
    route->table = table;
    if (if_indextoname(ifindex, route->interface_name) == NULL)
        return -1;
    return 0;
}

static int default_collect_routes(linux_ipv4_route_t *routes, size_t capacity,
                                  size_t *count)
{
    int socket_fd;
    struct sockaddr_nl address;
    struct {
        struct nlmsghdr header;
        struct rtmsg message;
    } request;
    unsigned int sequence = (unsigned int)getpid();
    char buffer[32768];

    if (routes == NULL || count == NULL || capacity == 0)
        return -1;
    *count = 0;
    socket_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (socket_fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(socket_fd);
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    request.header.nlmsg_type = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = sequence;
    request.message.rtm_family = AF_INET;
    request.message.rtm_table = RT_TABLE_UNSPEC;
    if (send(socket_fd, &request, request.header.nlmsg_len, 0) < 0) {
        close(socket_fd);
        return -1;
    }
    for (;;) {
        struct iovec vector;
        struct msghdr response;
        ssize_t received;
        struct nlmsghdr *header;
        int remaining;

        memset(&response, 0, sizeof(response));
        vector.iov_base = buffer;
        vector.iov_len = sizeof(buffer);
        response.msg_iov = &vector;
        response.msg_iovlen = 1;
        received = recvmsg(socket_fd, &response, MSG_TRUNC);
        if (received < 0) {
            if (errno == EINTR)
                continue;
            close(socket_fd);
            return -1;
        }
        if (received == 0 || received > (ssize_t)sizeof(buffer) ||
            (response.msg_flags & MSG_TRUNC) != 0) {
            close(socket_fd);
            return -1;
        }
        remaining = (int)received;
        for (header = (struct nlmsghdr *)buffer; NLMSG_OK(header, remaining);
             header = NLMSG_NEXT(header, remaining)) {
            struct rtmsg *message;
            int result;

            if (header->nlmsg_seq != sequence)
                continue;
            if (header->nlmsg_type == NLMSG_DONE) {
                close(socket_fd);
                return 0;
            }
            if (header->nlmsg_type == NLMSG_ERROR ||
                (header->nlmsg_flags & NLM_F_DUMP_INTR) != 0) {
                close(socket_fd);
                return -1;
            }
            message = (struct rtmsg *)NLMSG_DATA(header);
            if (message->rtm_family != AF_INET || message->rtm_type != RTN_UNICAST)
                continue;
            result = append_route(message, RTM_RTA(message), RTM_PAYLOAD(header),
                                  routes, capacity, count);
            if (result != 0) {
                close(socket_fd);
                return -1;
            }
        }
        if (remaining != 0) {
            close(socket_fd);
            return -1;
        }
    }
}

static int default_interface_has_ipv4(const char *interface_name,
                                      const linux_ipv4_cidr_t *expected)
{
    struct ifaddrs *addresses;
    struct ifaddrs *current;
    int found = 0;

    if (getifaddrs(&addresses) != 0)
        return 0;
    for (current = addresses; current != NULL; current = current->ifa_next) {
        struct sockaddr_in *address;
        struct sockaddr_in *mask;

        if (current->ifa_addr == NULL || current->ifa_netmask == NULL ||
            current->ifa_addr->sa_family != AF_INET ||
            strcmp(current->ifa_name, interface_name) != 0)
            continue;
        address = (struct sockaddr_in *)current->ifa_addr;
        mask = (struct sockaddr_in *)current->ifa_netmask;
        if (ntohl(address->sin_addr.s_addr) == expected->address &&
            ntohl(mask->sin_addr.s_addr) == expected->netmask) {
            found = 1;
            break;
        }
    }
    freeifaddrs(addresses);
    return found;
}

static int default_pidfile_read(const char *path, pid_t *pid)
{
    FILE *file;
    long value;
    char extra;

    if (pid == NULL)
        return -1;
    file = fopen(path, "r");
    if (file == NULL)
        return -1;
    if (fscanf(file, "%ld %c", &value, &extra) != 1 || value <= 1 ||
        value > 1L << 30) {
        fclose(file);
        return -1;
    }
    fclose(file);
    *pid = (pid_t)value;
    return 0;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static int default_pid_is_expected(pid_t pid, const char *expected_executable)
{
    char path[64];
    char executable[512];
    ssize_t length;

    if (pid <= 1 || expected_executable == NULL || kill(pid, 0) != 0)
        return 0;
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    length = readlink(path, executable, sizeof(executable) - 1);
    if (length <= 0)
        return 0;
    executable[length] = '\0';
    return strcmp(base_name(executable), base_name(expected_executable)) == 0;
}

static int inode_is_udp_listener(unsigned long inode, uint16_t port,
                                 uint32_t expected_address)
{
    FILE *file = fopen("/proc/net/udp", "r");
    char line[512];

    if (file == NULL)
        return 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned int address;
        unsigned int parsed_port;
        unsigned long parsed_inode;
        char *cursor = line;
        int field = 0;

        while (*cursor != '\0' && field < 9) {
            while (*cursor == ' ' || *cursor == '\t')
                ++cursor;
            if (*cursor == '\0' || *cursor == '\n')
                break;
            if (field == 1 && sscanf(cursor, "%8X:%4X", &address, &parsed_port) != 2)
                break;
            if (field == 9)
                break;
            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\n')
                ++cursor;
            ++field;
        }
        if (field < 9)
            continue;
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        if (sscanf(cursor, "%lu", &parsed_inode) == 1 && parsed_inode == inode &&
            parsed_port == port &&
            (address == 0 || ntohl(address) == expected_address)) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static int default_pid_has_udp_listener(pid_t pid, uint16_t port,
                                        uint32_t expected_address)
{
    char directory_path[64];
    DIR *directory;
    struct dirent *entry;
    int found = 0;

    snprintf(directory_path, sizeof(directory_path), "/proc/%ld/fd", (long)pid);
    directory = opendir(directory_path);
    if (directory == NULL)
        return 0;
    while ((entry = readdir(directory)) != NULL) {
        char link_path[384];
        char target[128];
        ssize_t length;
        unsigned long inode;

        if (entry->d_name[0] == '.')
            continue;
        snprintf(link_path, sizeof(link_path), "%s/%s", directory_path, entry->d_name);
        length = readlink(link_path, target, sizeof(target) - 1);
        if (length <= 0)
            continue;
        target[length] = '\0';
        if (sscanf(target, "socket:[%lu]", &inode) == 1 &&
            inode_is_udp_listener(inode, port, expected_address)) {
            found = 1;
            break;
        }
    }
    closedir(directory);
    return found;
}

static uint64_t default_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void default_sleep_ms(unsigned int milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000U;
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int default_interface_exists(const char *interface_name)
{
    return if_nametoindex(interface_name) != 0;
}

static int capture_iw_info(const char *interface_name, int *channel, int *is_ap)
{
    char output[4096];
    size_t used = 0;
    const char *const argv[] = {"iw", "dev", interface_name, "info", NULL};
    int exit_code;

    exit_code = linux_exec_argv(argv, output, sizeof(output));
    if (exit_code < 0 || exit_code != 0)
        return -1;
    if (channel != NULL) {
        char *position = strstr(output, "channel ");
        *channel = position == NULL ? 0 : atoi(position + 8);
    }
    if (is_ap != NULL)
        *is_ap = strstr(output, "type AP") != NULL || strstr(output, "type __ap") != NULL;
    return 0;
}

static int default_interface_is_ap(const char *interface_name)
{
    int is_ap = 0;
    return capture_iw_info(interface_name, NULL, &is_ap) == 0 && is_ap;
}

static int default_is_root(void)
{
    return geteuid() == 0;
}

static int default_binary_available(const char *executable)
{
    const char *path;
    char candidate[512];

    if (executable == NULL || executable[0] == '\0')
        return 0;
    if (strchr(executable, '/') != NULL)
        return access(executable, X_OK) == 0;
    path = getenv("PATH");
    if (path == NULL)
        return 0;
    while (*path != '\0') {
        const char *end = strchr(path, ':');
        size_t length = end == NULL ? strlen(path) : (size_t)(end - path);
        if (length + strlen(executable) + 2 < sizeof(candidate)) {
            snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, path,
                     executable);
            if (access(candidate, X_OK) == 0)
                return 1;
        }
        if (end == NULL)
            break;
        path = end + 1;
    }
    return 0;
}

static int default_sta_channel(const char *interface_name)
{
    int channel = 0;
    return capture_iw_info(interface_name, &channel, NULL) == 0 ? channel : 0;
}

static int default_ip_forward_enabled(void)
{
    FILE *file = fopen("/proc/sys/net/ipv4/ip_forward", "r");
    int enabled = 0;
    if (file == NULL)
        return 0;
    if (fscanf(file, "%d", &enabled) != 1)
        enabled = 0;
    fclose(file);
    return enabled == 1;
}

static const linux_hotspot_ops_t g_default_ops = {
    default_run_argv,
    default_collect_routes,
    default_interface_has_ipv4,
    default_pidfile_read,
    default_pid_is_expected,
    default_pid_has_udp_listener,
    default_monotonic_ms,
    default_sleep_ms,
    default_interface_exists,
    default_interface_is_ap,
    default_is_root,
    default_binary_available,
    default_sta_channel,
    default_ip_forward_enabled,
};

const linux_hotspot_ops_t *linux_hotspot_default_ops(void)
{
    return &g_default_ops;
}

#else

const linux_hotspot_ops_t *linux_hotspot_default_ops(void)
{
    return NULL;
}

#endif
