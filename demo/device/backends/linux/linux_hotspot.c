#include "linux_hotspot.h"

#include "linux_hotspot_config.h"
#include "linux_hotspot_linux_ops.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TAG "LINUX_AP"
#define ROUTE_CAPACITY 512
#define DHCP_READY_ATTEMPTS 20
#define HOSTAPD_READY_ATTEMPTS 20
#define NAT_COMMENT "modu-provision-nat"

typedef struct linux_hotspot_runtime {
    int interface_created;
    int hostapd_config_created;
    int hostapd_started;
    int address_added;
    int dnsmasq_started;
    int nat_owned;
} linux_hotspot_runtime_t;

struct linux_hotspot {
    linux_hotspot_cfg_t cfg;
    linux_hotspot_plan_t plan;
    linux_hotspot_runtime_t runtime;
    const linux_hotspot_ops_t *ops;
    int cfg_loaded;
    int active;
    int lock_fd;
    char lock_path[256];
    char hostapd_conf[256];
    char hostapd_pid[256];
    char dnsmasq_pid[256];
};

static const linux_hotspot_ops_t *ops(const linux_hotspot_t *hotspot)
{
    return hotspot->ops != NULL ? hotspot->ops : linux_hotspot_default_ops();
}

static int ops_complete(const linux_hotspot_ops_t *value)
{
    return value != NULL && value->run_argv != NULL &&
           value->collect_routes != NULL && value->interface_has_ipv4 != NULL &&
           value->pidfile_read != NULL && value->pid_is_expected != NULL &&
           value->pid_has_udp_listener != NULL && value->monotonic_ms != NULL &&
           value->sleep_ms != NULL && value->interface_exists != NULL &&
           value->interface_is_ap != NULL && value->is_root != NULL &&
           value->binary_available != NULL && value->sta_channel != NULL &&
           value->ip_forward_enabled != NULL;
}

static int make_path(linux_hotspot_t *hotspot, char *output, size_t capacity,
                     const char *name)
{
    int length = snprintf(output, capacity, "%s/%s", hotspot->cfg.work_dir, name);
    return length > 0 && (size_t)length < capacity ? DEMO_OK : DEMO_ERR_INVAL;
}

static int acquire_lock(linux_hotspot_t *hotspot)
{
    char pid_text[32];
    int length;

    if (make_path(hotspot, hotspot->lock_path, sizeof(hotspot->lock_path),
                  "modu_linux_hotspot.lock") != DEMO_OK)
        return DEMO_ERR_INVAL;
    hotspot->lock_fd = open(hotspot->lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (hotspot->lock_fd < 0 || flock(hotspot->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (hotspot->lock_fd >= 0)
            close(hotspot->lock_fd);
        hotspot->lock_fd = -1;
        return DEMO_ERR;
    }
    length = snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)getpid());
    if (ftruncate(hotspot->lock_fd, 0) != 0 ||
        lseek(hotspot->lock_fd, 0, SEEK_SET) < 0 ||
        write(hotspot->lock_fd, pid_text, (size_t)length) != length ||
        fsync(hotspot->lock_fd) != 0) {
        flock(hotspot->lock_fd, LOCK_UN);
        close(hotspot->lock_fd);
        hotspot->lock_fd = -1;
        return DEMO_ERR;
    }
    return DEMO_OK;
}

static int credentials_valid(const char *ssid, const char *password)
{
    size_t ssid_length;
    size_t password_length;

    if (ssid == NULL || password == NULL || strchr(ssid, '\n') != NULL ||
        strchr(ssid, '\r') != NULL || strchr(password, '\n') != NULL ||
        strchr(password, '\r') != NULL)
        return 0;
    ssid_length = strlen(ssid);
    password_length = strlen(password);
    return ssid_length >= 1 && ssid_length <= 32 && password_length >= 8 &&
           password_length <= 63;
}

static int write_hostapd_config(linux_hotspot_t *hotspot, const char *ssid,
                                const char *password, int channel)
{
    int file_fd;
    FILE *file;
    int result = DEMO_ERR;

    file_fd = open(hotspot->hostapd_conf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (file_fd < 0)
        return DEMO_ERR;
    hotspot->runtime.hostapd_config_created = 1;
    file = fdopen(file_fd, "w");
    if (file == NULL) {
        close(file_fd);
        return DEMO_ERR;
    }
    if (fprintf(file,
                "interface=%s\nssid=%s\nhw_mode=g\nchannel=%d\n"
                "wmm_enabled=1\nauth_algs=1\nwpa=2\nwpa_passphrase=%s\n"
                "wpa_key_mgmt=WPA-PSK\nwpa_pairwise=TKIP\nrsn_pairwise=CCMP\n"
                "logger_stdout=-1\n",
                hotspot->cfg.ap_interface, ssid, channel, password) > 0 &&
        fflush(file) == 0 && fsync(file_fd) == 0)
        result = DEMO_OK;
    if (fclose(file) != 0)
        result = DEMO_ERR;
    return result;
}

static int run(linux_hotspot_t *hotspot, const char *const argv[])
{
    return ops(hotspot)->run_argv(argv);
}

static int terminate_owned_pidfile(linux_hotspot_t *hotspot,
                                   const char *pidfile, const char *executable)
{
    pid_t pid;
    char pid_text[32];
    const char *const terminate[] = {"kill", "-TERM", pid_text, NULL};

    if (ops(hotspot)->pidfile_read(pidfile, &pid) != 0)
        return DEMO_OK;
    if (!ops(hotspot)->pid_is_expected(pid, executable)) {
        LOG_W(TAG, "pidfile %s 指向非预期进程，拒绝终止", pidfile);
        return DEMO_ERR_INVAL;
    }
    snprintf(pid_text, sizeof(pid_text), "%ld", (long)pid);
    return run(hotspot, terminate) == 0 ? DEMO_OK : DEMO_ERR;
}

static void remove_owned_file(const char *path)
{
    if (path[0] != '\0' && unlink(path) != 0 && errno != ENOENT)
        LOG_W(TAG, "清理文件失败: %s", path);
}

static void format_plan(linux_hotspot_t *hotspot, char *ap, char *start,
                        char *end, char *mask, char *cidr, size_t capacity)
{
    linux_ipv4_format(hotspot->plan.ap.address, ap, capacity);
    linux_ipv4_format(hotspot->plan.dhcp_start, start, capacity);
    linux_ipv4_format(hotspot->plan.dhcp_end, end, capacity);
    linux_netmask_format(hotspot->plan.ap.netmask, mask, capacity);
    snprintf(cidr, capacity, "%s/%u", ap, (unsigned)hotspot->plan.ap.prefix_length);
}

static void format_plan_cidr(linux_hotspot_t *hotspot, char *cidr, size_t capacity)
{
    char ap[32];
    linux_ipv4_format(hotspot->plan.ap.address, ap, sizeof(ap));
    snprintf(cidr, capacity, "%s/%u", ap, (unsigned)hotspot->plan.ap.prefix_length);
}

static int nat_command(linux_hotspot_t *hotspot, const char *operation)
{
    char network[32];
    char cidr[40];
    const char *const argv[] = {
        "iptables", "-t", "nat", operation, "POSTROUTING", "-s", cidr,
        "-o", hotspot->cfg.sta_interface, "-m", "comment", "--comment", NAT_COMMENT,
        "-j", "MASQUERADE", NULL
    };

    linux_ipv4_format(hotspot->plan.ap.network, network, sizeof(network));
    snprintf(cidr, sizeof(cidr), "%s/%u", network,
             (unsigned)hotspot->plan.ap.prefix_length);
    return run(hotspot, argv);
}

static void rollback(linux_hotspot_t *hotspot)
{
    const char *delete_address[] = {"ip", "addr", "del", NULL, "dev",
                                    hotspot->cfg.ap_interface, NULL};
    const char *const delete_interface[] = {"iw", "dev", hotspot->cfg.ap_interface,
                                            "del", NULL};
    char cidr[32];
    int attempts;

    if (hotspot->runtime.nat_owned) {
        for (attempts = 0;
             attempts < 64 && nat_command(hotspot, "-D") == 0; ++attempts) {
        }
        hotspot->runtime.nat_owned = 0;
    }
    if (hotspot->runtime.dnsmasq_started) {
        int cleanup_result = terminate_owned_pidfile(hotspot, hotspot->dnsmasq_pid,
                                                     hotspot->cfg.dnsmasq_bin);
        if (cleanup_result != DEMO_OK)
            LOG_W(TAG, "dnsmasq 清理未完成");
        if (cleanup_result != DEMO_ERR_INVAL)
            remove_owned_file(hotspot->dnsmasq_pid);
        hotspot->runtime.dnsmasq_started = 0;
    }
    if (hotspot->runtime.address_added) {
        format_plan_cidr(hotspot, cidr, sizeof(cidr));
        delete_address[3] = cidr;
        if (run(hotspot, delete_address) != 0)
            LOG_W(TAG, "AP 地址清理失败");
        hotspot->runtime.address_added = 0;
    }
    if (hotspot->runtime.hostapd_started) {
        int cleanup_result = terminate_owned_pidfile(hotspot, hotspot->hostapd_pid,
                                                     hotspot->cfg.hostapd_bin);
        if (cleanup_result != DEMO_OK)
            LOG_W(TAG, "hostapd 清理未完成");
        if (cleanup_result != DEMO_ERR_INVAL)
            remove_owned_file(hotspot->hostapd_pid);
        hotspot->runtime.hostapd_started = 0;
    }
    if (hotspot->runtime.hostapd_config_created) {
        remove_owned_file(hotspot->hostapd_conf);
        hotspot->runtime.hostapd_config_created = 0;
    }
    if (hotspot->runtime.interface_created) {
        if (run(hotspot, delete_interface) != 0)
            LOG_W(TAG, "AP 接口清理失败");
        hotspot->runtime.interface_created = 0;
    }
    hotspot->active = 0;
}

int linux_hotspot_create(linux_hotspot_t **out, const char *config_path,
                         const linux_hotspot_ops_t *ops_override,
                         const char *sta_interface_override,
                         char *error, size_t error_capacity)
{
    linux_hotspot_t *hotspot;
    const linux_hotspot_ops_t *selected_ops;
    int result;
    char load_error[256];

    if (out == NULL)
        return DEMO_ERR_INVAL;
    *out = NULL;
    selected_ops = ops_override != NULL ? ops_override : linux_hotspot_default_ops();
    if (!ops_complete(selected_ops))
        return DEMO_ERR_INVAL;
    if (config_path == NULL || config_path[0] == '\0')
        config_path = LINUX_HOTSPOT_DEFAULT_CONFIG;

    hotspot = (linux_hotspot_t *)calloc(1, sizeof(*hotspot));
    if (hotspot == NULL)
        return DEMO_ERR_NOMEM;
    hotspot->lock_fd = -1;
    hotspot->ops = selected_ops;

    result = linux_hotspot_cfg_load(&hotspot->cfg, config_path,
                                    load_error, sizeof(load_error));
    if (result != DEMO_OK) {
        if (error != NULL && error_capacity > 0)
            snprintf(error, error_capacity, "热点配置无效: %s", load_error);
        free(hotspot);
        return result;
    }
    if (sta_interface_override != NULL && sta_interface_override[0] != '\0') {
        snprintf(hotspot->cfg.sta_interface, sizeof(hotspot->cfg.sta_interface),
                 "%s", sta_interface_override);
        result = linux_hotspot_cfg_validate(&hotspot->cfg, load_error,
                                            sizeof(load_error));
        if (result != DEMO_OK) {
            if (error != NULL && error_capacity > 0)
                snprintf(error, error_capacity, "STA 网卡覆盖无效: %s", load_error);
            free(hotspot);
            return DEMO_ERR_INVAL;
        }
    }
    if (!ops(hotspot)->is_root()) {
        LOG_E(TAG, "真实 Linux 网络后端需要 root 权限");
        if (error != NULL && error_capacity > 0)
            snprintf(error, error_capacity, "真实 Linux 网络后端需要 root 权限");
        free(hotspot);
        return DEMO_ERR;
    }
    if (make_path(hotspot, hotspot->hostapd_conf, sizeof(hotspot->hostapd_conf),
                  "hostapd_modu.conf") != DEMO_OK ||
        make_path(hotspot, hotspot->hostapd_pid, sizeof(hotspot->hostapd_pid),
                  "hostapd_modu.pid") != DEMO_OK ||
        make_path(hotspot, hotspot->dnsmasq_pid, sizeof(hotspot->dnsmasq_pid),
                  "dnsmasq_modu_ap.pid") != DEMO_OK) {
        free(hotspot);
        return DEMO_ERR_INVAL;
    }
    if (acquire_lock(hotspot) != DEMO_OK) {
        LOG_E(TAG, "无法获取真实热点单实例锁: %s", hotspot->lock_path);
        if (error != NULL && error_capacity > 0)
            snprintf(error, error_capacity, "无法获取真实热点单实例锁: %s",
                     hotspot->lock_path);
        free(hotspot);
        return DEMO_ERR;
    }
    memset(&hotspot->runtime, 0, sizeof(hotspot->runtime));
    hotspot->cfg_loaded = 1;
    LOG_I(TAG, "热点配置已加载：sta=%s ap=%s nat=%d", hotspot->cfg.sta_interface,
          hotspot->cfg.ap_interface, hotspot->cfg.nat);
    *out = hotspot;
    return DEMO_OK;
}

void linux_hotspot_destroy(linux_hotspot_t *hotspot)
{
    if (hotspot == NULL)
        return;
    if (hotspot->cfg_loaded)
        linux_hotspot_stop(hotspot);
    if (hotspot->lock_fd >= 0) {
        flock(hotspot->lock_fd, LOCK_UN);
        close(hotspot->lock_fd);
    }
    hotspot->lock_fd = -1;
    hotspot->cfg_loaded = 0;
    hotspot->active = 0;
    memset(&hotspot->cfg, 0, sizeof(hotspot->cfg));
    memset(&hotspot->plan, 0, sizeof(hotspot->plan));
    memset(&hotspot->runtime, 0, sizeof(hotspot->runtime));
    free(hotspot);
}

int linux_hotspot_start(linux_hotspot_t *hotspot, const char *ssid,
                        const char *password)
{
    linux_ipv4_route_t routes[ROUTE_CAPACITY];
    size_t route_count = 0;
    char error[256];
    char ap[32];
    char start[32];
    char end[32];
    char mask[32];
    char cidr[32];
    char range[128];
    char router[64];
    char server_id[64];
    char listen_address[64];
    char interface_arg[64];
    char pidfile_arg[320];
    int channel;
    int attempt;
    int nat_check;
    size_t route_index;
    pid_t hostapd_pid;
    pid_t dnsmasq_pid;
    const char *const create_interface[] = {"iw", "dev", hotspot->cfg.sta_interface,
                                            "interface", "add", hotspot->cfg.ap_interface,
                                            "type", "__ap", NULL};
    const char *const unmanaged[] = {"nmcli", "dev", "set", hotspot->cfg.ap_interface,
                                     "managed", "no", NULL};
    const char *hostapd_argv[] = {hotspot->cfg.hostapd_bin, "-B", "-P",
                                  hotspot->hostapd_pid, hotspot->hostapd_conf, NULL};
    const char *address_argv[] = {"ip", "addr", "add", cidr, "dev",
                                  hotspot->cfg.ap_interface, NULL};
    const char *dnsmasq_argv[] = {
        hotspot->cfg.dnsmasq_bin, "--conf-file=/dev/null", "--port=0",
        "--bind-interfaces", interface_arg, listen_address,
        "--dhcp-authoritative", range, router, server_id, pidfile_arg,
        "--log-dhcp", NULL
    };

    if (hotspot == NULL || !hotspot->cfg_loaded || !hotspot->cfg.enable ||
        !credentials_valid(ssid, password))
        return DEMO_ERR_INVAL;
    if (!ops(hotspot)->is_root()) {
        LOG_E(TAG, "启动真实热点需要 root 权限");
        return DEMO_ERR;
    }
    if (!ops(hotspot)->binary_available("iw") ||
        !ops(hotspot)->binary_available("ip") ||
        !ops(hotspot)->binary_available("nmcli") ||
        !ops(hotspot)->binary_available(hotspot->cfg.hostapd_bin) ||
        !ops(hotspot)->binary_available(hotspot->cfg.dnsmasq_bin) ||
        (hotspot->cfg.nat && !ops(hotspot)->binary_available("iptables"))) {
        LOG_E(TAG, "热点依赖程序不可用");
        return DEMO_ERR;
    }
    if (hotspot->active)
        linux_hotspot_stop(hotspot);
    if (ops(hotspot)->collect_routes(routes, ROUTE_CAPACITY, &route_count) != 0) {
        LOG_E(TAG, "直连路由采集失败，拒绝启动热点");
        return DEMO_ERR;
    }
    for (route_index = 0; route_index < route_count; ++route_index) {
        char route_network[32];
        linux_ipv4_format(routes[route_index].destination.network, route_network,
                          sizeof(route_network));
        LOG_I(TAG, "直连路由：%s/%u dev=%s table=%u", route_network,
              (unsigned)routes[route_index].destination.prefix_length,
              routes[route_index].interface_name, routes[route_index].table);
    }
    if (linux_hotspot_select_plan(&hotspot->cfg, routes, route_count,
                                  &hotspot->plan, error, sizeof(error)) != DEMO_OK) {
        LOG_E(TAG, "热点地址计划失败: %s", error);
        return DEMO_ERR;
    }
    LOG_I(TAG, "热点地址计划：候选=%d fallback=%d", hotspot->plan.candidate_index,
          hotspot->plan.used_fallback);
    if (ops(hotspot)->interface_exists(hotspot->cfg.ap_interface)) {
        LOG_E(TAG, "AP 接口 %s 已存在且所有权未知", hotspot->cfg.ap_interface);
        return DEMO_ERR;
    }
    if (access(hotspot->hostapd_conf, F_OK) == 0 ||
        access(hotspot->hostapd_pid, F_OK) == 0 ||
        access(hotspot->dnsmasq_pid, F_OK) == 0) {
        LOG_E(TAG, "热点工作文件已存在且所有权未知，拒绝覆盖");
        return DEMO_ERR;
    }
    channel = hotspot->cfg.channel > 0 ? hotspot->cfg.channel
                                       : ops(hotspot)->sta_channel(hotspot->cfg.sta_interface);
    if (channel <= 0)
        channel = 1;
    if (run(hotspot, create_interface) != 0) {
        LOG_E(TAG, "创建 AP 接口失败");
        goto fail;
    }
    hotspot->runtime.interface_created = 1;
    if (run(hotspot, unmanaged) != 0) {
        LOG_E(TAG, "设置 AP 接口 unmanaged 失败");
        goto fail;
    }
    if (write_hostapd_config(hotspot, ssid, password, channel) != DEMO_OK) {
        LOG_E(TAG, "写入 hostapd 配置失败");
        goto fail;
    }
    if (run(hotspot, hostapd_argv) != 0) {
        LOG_E(TAG, "hostapd 启动失败");
        goto fail;
    }
    hotspot->runtime.hostapd_started = 1;
    for (attempt = 0; attempt < HOSTAPD_READY_ATTEMPTS; ++attempt) {
        if (ops(hotspot)->pidfile_read(hotspot->hostapd_pid, &hostapd_pid) == 0 &&
            ops(hotspot)->pid_is_expected(hostapd_pid, hotspot->cfg.hostapd_bin))
            break;
        if (attempt + 1 < HOSTAPD_READY_ATTEMPTS)
            ops(hotspot)->sleep_ms(100);
    }
    if (attempt == HOSTAPD_READY_ATTEMPTS) {
        LOG_E(TAG, "hostapd pidfile 或进程身份无效");
        goto fail;
    }
    ops(hotspot)->sleep_ms(250);
    if (!ops(hotspot)->interface_is_ap(hotspot->cfg.ap_interface)) {
        LOG_E(TAG, "hostapd 未使接口进入 AP 模式");
        goto fail;
    }
    format_plan(hotspot, ap, start, end, mask, cidr, sizeof(cidr));
    if (run(hotspot, address_argv) != 0) {
        LOG_E(TAG, "配置 AP 地址失败");
        goto fail;
    }
    hotspot->runtime.address_added = 1;
    if (!ops(hotspot)->interface_has_ipv4(hotspot->cfg.ap_interface,
                                          &hotspot->plan.ap)) {
        LOG_E(TAG, "AP 地址或掩码回读不匹配");
        goto fail;
    }
    snprintf(interface_arg, sizeof(interface_arg), "--interface=%s",
             hotspot->cfg.ap_interface);
    snprintf(listen_address, sizeof(listen_address), "--listen-address=%s", ap);
    snprintf(range, sizeof(range), "--dhcp-range=%s,%s,%s,12h", start, end, mask);
    snprintf(router, sizeof(router), "--dhcp-option=option:router,%s", ap);
    snprintf(server_id, sizeof(server_id), "--dhcp-option=54,%s", ap);
    snprintf(pidfile_arg, sizeof(pidfile_arg), "--pid-file=%s",
             hotspot->dnsmasq_pid);
    if (run(hotspot, dnsmasq_argv) != 0) {
        LOG_E(TAG, "dnsmasq 启动失败");
        goto fail;
    }
    hotspot->runtime.dnsmasq_started = 1;
    for (attempt = 0; attempt < DHCP_READY_ATTEMPTS; ++attempt) {
        if (ops(hotspot)->pidfile_read(hotspot->dnsmasq_pid, &dnsmasq_pid) == 0 &&
            ops(hotspot)->pid_is_expected(dnsmasq_pid, hotspot->cfg.dnsmasq_bin) &&
            ops(hotspot)->pid_has_udp_listener(dnsmasq_pid, 67,
                                               hotspot->plan.ap.address))
            break;
        if (attempt + 1 < DHCP_READY_ATTEMPTS)
            ops(hotspot)->sleep_ms(100);
    }
    if (attempt == DHCP_READY_ATTEMPTS) {
        LOG_E(TAG, "dnsmasq UDP 67 readiness 超时");
        goto fail;
    }
    LOG_I(TAG, "部署环境必须放行 %s UDP 67 和 TCP 5935；程序不修改过滤规则",
          hotspot->cfg.ap_interface);
    if (hotspot->cfg.nat) {
        if (!ops(hotspot)->ip_forward_enabled()) {
            LOG_E(TAG, "NAT 已启用但 ip_forward != 1");
            goto fail;
        }
        nat_check = nat_command(hotspot, "-C");
        if (nat_check != 0) {
            if (nat_command(hotspot, "-A") != 0) {
                LOG_E(TAG, "添加精确 NAT 规则失败");
                goto fail;
            }
            hotspot->runtime.nat_owned = 1;
        }
    }
    hotspot->active = 1;
    LOG_I(TAG, "真实热点已就绪：信道=%d 网关=%s/%u", channel, ap,
          (unsigned)hotspot->plan.ap.prefix_length);
    return DEMO_OK;

fail:
    rollback(hotspot);
    return DEMO_ERR;
}

int linux_hotspot_stop(linux_hotspot_t *hotspot)
{
    if (hotspot == NULL || !hotspot->cfg_loaded)
        return DEMO_OK;
    rollback(hotspot);
    return DEMO_OK;
}

int linux_hotspot_is_active(const linux_hotspot_t *hotspot)
{
    return hotspot != NULL && hotspot->active;
}

const char *linux_hotspot_sta_interface(const linux_hotspot_t *hotspot)
{
    return hotspot != NULL && hotspot->cfg_loaded ? hotspot->cfg.sta_interface : "";
}
