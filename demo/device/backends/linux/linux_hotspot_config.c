#include "linux_hotspot_config.h"

#include "cJSON.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINUX_HOTSPOT_CONFIG_MAX_SIZE (1024U * 1024U)

static void set_error(char *error, size_t capacity, const char *format, ...)
{
    va_list args;

    if (error == NULL || capacity == 0)
        return;
    va_start(args, format);
    vsnprintf(error, capacity, format, args);
    va_end(args);
    error[capacity - 1] = '\0';
}

static void clear_error(char *error, size_t capacity)
{
    if (error != NULL && capacity > 0)
        error[0] = '\0';
}

static int copy_json_string(const cJSON *root, const char *name,
                            char *destination, size_t capacity,
                            char *error, size_t error_capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    size_t length;

    if (item == NULL)
        return DEMO_OK;
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        set_error(error, error_capacity, "字段 %s 必须是字符串", name);
        return DEMO_ERR_INVAL;
    }
    length = strlen(item->valuestring);
    if (length >= capacity) {
        set_error(error, error_capacity, "字段 %s 字符串过长", name);
        return DEMO_ERR_INVAL;
    }
    memcpy(destination, item->valuestring, length + 1);
    return DEMO_OK;
}

static int load_bool(const cJSON *root, const char *name, int *value,
                     char *error, size_t error_capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (item == NULL)
        return DEMO_OK;
    if (!cJSON_IsBool(item)) {
        set_error(error, error_capacity, "字段 %s 必须是布尔值", name);
        return DEMO_ERR_INVAL;
    }
    *value = cJSON_IsTrue(item) ? 1 : 0;
    return DEMO_OK;
}

static int load_int(const cJSON *root, const char *name, int *value,
                    char *error, size_t error_capacity)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (item == NULL)
        return DEMO_OK;
    if (!cJSON_IsNumber(item) || item->valuedouble != (double)item->valueint) {
        set_error(error, error_capacity, "字段 %s 必须是整数", name);
        return DEMO_ERR_INVAL;
    }
    *value = item->valueint;
    return DEMO_OK;
}

static int valid_interface_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;

    if (*cursor == '\0')
        return 0;
    while (*cursor != '\0') {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
              (*cursor >= 'A' && *cursor <= 'Z') ||
              (*cursor >= '0' && *cursor <= '9') || *cursor == '_' ||
              *cursor == '-' || *cursor == '.'))
            return 0;
        ++cursor;
    }
    return 1;
}

void linux_hotspot_cfg_defaults(linux_hotspot_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enable = 1;
    snprintf(cfg->sta_interface, sizeof(cfg->sta_interface), "wlP2p33s0");
    snprintf(cfg->ap_interface, sizeof(cfg->ap_interface), "ap0");
    snprintf(cfg->subnet, sizeof(cfg->subnet), "10.42.0.1");
    cfg->prefix_length = 24;
    snprintf(cfg->dhcp_start, sizeof(cfg->dhcp_start), "10.42.0.100");
    snprintf(cfg->dhcp_end, sizeof(cfg->dhcp_end), "10.42.0.200");
    cfg->channel = 0;
    cfg->nat = 0;
    snprintf(cfg->hostapd_bin, sizeof(cfg->hostapd_bin), "hostapd");
    snprintf(cfg->dnsmasq_bin, sizeof(cfg->dnsmasq_bin), "dnsmasq");
    snprintf(cfg->work_dir, sizeof(cfg->work_dir), "/tmp");
}

int linux_ipv4_cidr_parse(const char *address, int prefix_length,
                          linux_ipv4_cidr_t *out)
{
    struct in_addr parsed;
    uint32_t mask;

    if (out == NULL)
        return DEMO_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (address == NULL || prefix_length < 0 || prefix_length > 32 ||
        inet_pton(AF_INET, address, &parsed) != 1)
        return DEMO_ERR_INVAL;

    mask = prefix_length == 0 ? 0U : UINT32_MAX << (32 - prefix_length);
    out->address = ntohl(parsed.s_addr);
    out->network = out->address & mask;
    out->netmask = mask;
    out->broadcast = out->network | ~mask;
    out->prefix_length = (uint8_t)prefix_length;
    return DEMO_OK;
}

void linux_ipv4_format(uint32_t host_order_ip, char *out, size_t capacity)
{
    struct in_addr address;

    if (out == NULL || capacity == 0)
        return;
    out[0] = '\0';
    address.s_addr = htonl(host_order_ip);
    if (inet_ntop(AF_INET, &address, out, capacity) == NULL)
        out[0] = '\0';
}

void linux_netmask_format(uint32_t host_order_mask, char *out, size_t capacity)
{
    linux_ipv4_format(host_order_mask, out, capacity);
}

int linux_hotspot_cfg_validate(const linux_hotspot_cfg_t *cfg,
                               char *error, size_t error_capacity)
{
    linux_ipv4_cidr_t ap;
    linux_ipv4_cidr_t start;
    linux_ipv4_cidr_t end;
    struct stat work_dir_stat;

    clear_error(error, error_capacity);
    if (cfg == NULL) {
        set_error(error, error_capacity, "配置指针为空");
        return DEMO_ERR_INVAL;
    }
    if (!valid_interface_name(cfg->sta_interface)) {
        set_error(error, error_capacity, "字段 sta_interface 非法");
        return DEMO_ERR_INVAL;
    }
    if (!valid_interface_name(cfg->ap_interface)) {
        set_error(error, error_capacity, "字段 ap_interface 非法");
        return DEMO_ERR_INVAL;
    }
    if (cfg->prefix_length < 16 || cfg->prefix_length > 30) {
        set_error(error, error_capacity, "字段 prefix_length 必须为 16 至 30");
        return DEMO_ERR_INVAL;
    }
    if (linux_ipv4_cidr_parse(cfg->subnet, cfg->prefix_length, &ap) != DEMO_OK) {
        set_error(error, error_capacity, "字段 subnet 不是合法 IPv4");
        return DEMO_ERR_INVAL;
    }
    if (linux_ipv4_cidr_parse(cfg->dhcp_start, cfg->prefix_length, &start) != DEMO_OK) {
        set_error(error, error_capacity, "字段 dhcp_start 不是合法 IPv4");
        return DEMO_ERR_INVAL;
    }
    if (linux_ipv4_cidr_parse(cfg->dhcp_end, cfg->prefix_length, &end) != DEMO_OK) {
        set_error(error, error_capacity, "字段 dhcp_end 不是合法 IPv4");
        return DEMO_ERR_INVAL;
    }
    if (ap.address == ap.network || ap.address == ap.broadcast) {
        set_error(error, error_capacity, "字段 subnet 不能是网络或广播地址");
        return DEMO_ERR_INVAL;
    }
    if (start.network != ap.network || end.network != ap.network) {
        set_error(error, error_capacity, "DHCP 池必须与 subnet 同子网");
        return DEMO_ERR_INVAL;
    }
    if (start.address > end.address) {
        set_error(error, error_capacity, "字段 dhcp_start 不能大于 dhcp_end");
        return DEMO_ERR_INVAL;
    }
    if (start.address == start.network || end.address == end.broadcast ||
        (start.address <= ap.address && ap.address <= end.address)) {
        set_error(error, error_capacity, "DHCP 池不能包含 AP、网络或广播地址");
        return DEMO_ERR_INVAL;
    }
    if (cfg->work_dir[0] == '\0' || stat(cfg->work_dir, &work_dir_stat) != 0 ||
        !S_ISDIR(work_dir_stat.st_mode) || access(cfg->work_dir, R_OK | W_OK | X_OK) != 0) {
        set_error(error, error_capacity, "字段 work_dir 不存在或不可访问: %s",
                  cfg->work_dir);
        return DEMO_ERR_INVAL;
    }
    return DEMO_OK;
}

int linux_hotspot_cfg_load(linux_hotspot_cfg_t *cfg, const char *path,
                           char *error, size_t error_capacity)
{
    FILE *file = NULL;
    char *buffer = NULL;
    long file_size;
    size_t bytes_read;
    cJSON *root = NULL;
    linux_hotspot_cfg_t loaded;
    int result = DEMO_ERR_INVAL;

    clear_error(error, error_capacity);
    if (cfg == NULL) {
        set_error(error, error_capacity, "配置输出指针为空");
        return DEMO_ERR_INVAL;
    }
    memset(cfg, 0, sizeof(*cfg));
    if (path == NULL || path[0] == '\0')
        path = LINUX_HOTSPOT_DEFAULT_CONFIG;
    linux_hotspot_cfg_defaults(&loaded);

    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_capacity, "无法读取配置 %s: %s", path, strerror(errno));
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        set_error(error, error_capacity, "无法确定配置大小: %s", path);
        goto cleanup;
    }
    if (file_size == 0 || (unsigned long)file_size > LINUX_HOTSPOT_CONFIG_MAX_SIZE) {
        set_error(error, error_capacity, "配置 %s 必须非空且不超过 1 MiB", path);
        goto cleanup;
    }
    buffer = (char *)malloc((size_t)file_size + 1);
    if (buffer == NULL) {
        result = DEMO_ERR_NOMEM;
        set_error(error, error_capacity, "读取配置 %s 时内存不足", path);
        goto cleanup;
    }
    bytes_read = fread(buffer, 1, (size_t)file_size, file);
    if (bytes_read != (size_t)file_size || ferror(file)) {
        set_error(error, error_capacity, "配置读取不完整: %s", path);
        goto cleanup;
    }
    buffer[file_size] = '\0';
    root = cJSON_ParseWithLengthOpts(buffer, (size_t)file_size + 1, NULL, 1);
    if (root == NULL || !cJSON_IsObject(root)) {
        set_error(error, error_capacity, "配置必须是合法 JSON object: %s", path);
        goto cleanup;
    }

#define LOAD_STRING(field) do { \
    result = copy_json_string(root, #field, loaded.field, sizeof(loaded.field), \
                              error, error_capacity); \
    if (result != DEMO_OK) goto cleanup; \
} while (0)
#define LOAD_BOOL(field) do { \
    result = load_bool(root, #field, &loaded.field, error, error_capacity); \
    if (result != DEMO_OK) goto cleanup; \
} while (0)
#define LOAD_INT(field) do { \
    result = load_int(root, #field, &loaded.field, error, error_capacity); \
    if (result != DEMO_OK) goto cleanup; \
} while (0)
    LOAD_BOOL(enable);
    LOAD_STRING(sta_interface);
    LOAD_STRING(ap_interface);
    LOAD_STRING(subnet);
    LOAD_INT(prefix_length);
    LOAD_STRING(dhcp_start);
    LOAD_STRING(dhcp_end);
    LOAD_INT(channel);
    LOAD_BOOL(nat);
    LOAD_STRING(hostapd_bin);
    LOAD_STRING(dnsmasq_bin);
    LOAD_STRING(work_dir);
#undef LOAD_STRING
#undef LOAD_BOOL
#undef LOAD_INT

    result = linux_hotspot_cfg_validate(&loaded, error, error_capacity);
    if (result == DEMO_OK)
        *cfg = loaded;

cleanup:
    cJSON_Delete(root);
    free(buffer);
    if (file != NULL)
        fclose(file);
    if (result != DEMO_OK)
        memset(cfg, 0, sizeof(*cfg));
    return result;
}

static int cidr_overlaps(const linux_ipv4_cidr_t *left,
                         const linux_ipv4_cidr_t *right)
{
    return left->network <= right->broadcast &&
           right->network <= left->broadcast;
}

int linux_hotspot_select_plan(const linux_hotspot_cfg_t *cfg,
                              const linux_ipv4_route_t *routes,
                              size_t route_count,
                              linux_hotspot_plan_t *out,
                              char *error, size_t error_capacity)
{
    static const char *fallback_addresses[] = {
        "10.43.0.1", "172.31.250.1", "192.168.250.1"
    };
    linux_ipv4_cidr_t preferred;
    linux_ipv4_cidr_t start;
    linux_ipv4_cidr_t end;
    uint32_t start_offset;
    uint32_t end_offset;
    size_t candidate_index;

    clear_error(error, error_capacity);
    if (out == NULL)
        return DEMO_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (cfg == NULL || (route_count > 0 && routes == NULL)) {
        set_error(error, error_capacity, "计划输入指针为空");
        return DEMO_ERR_INVAL;
    }
    if (linux_hotspot_cfg_validate(cfg, error, error_capacity) != DEMO_OK)
        return DEMO_ERR_INVAL;
    linux_ipv4_cidr_parse(cfg->subnet, cfg->prefix_length, &preferred);
    linux_ipv4_cidr_parse(cfg->dhcp_start, cfg->prefix_length, &start);
    linux_ipv4_cidr_parse(cfg->dhcp_end, cfg->prefix_length, &end);
    start_offset = start.address - preferred.network;
    end_offset = end.address - preferred.network;

    for (candidate_index = 0; candidate_index < 4; ++candidate_index) {
        linux_ipv4_cidr_t candidate;
        uint32_t relocated_start;
        uint32_t relocated_end;
        size_t route_index;
        int conflict = 0;

        if (candidate_index == 0) {
            candidate = preferred;
        } else {
            linux_ipv4_cidr_parse(fallback_addresses[candidate_index - 1], 24,
                                  &candidate);
        }
        if (start_offset > candidate.broadcast - candidate.network ||
            end_offset > candidate.broadcast - candidate.network)
            continue;
        relocated_start = candidate.network + start_offset;
        relocated_end = candidate.network + end_offset;
        if (relocated_start == candidate.network ||
            relocated_end == candidate.broadcast ||
            (relocated_start <= candidate.address &&
             candidate.address <= relocated_end))
            continue;
        for (route_index = 0; route_index < route_count; ++route_index) {
            if (cidr_overlaps(&candidate, &routes[route_index].destination)) {
                conflict = 1;
                break;
            }
        }
        if (!conflict) {
            out->ap = candidate;
            out->dhcp_start = relocated_start;
            out->dhcp_end = relocated_end;
            out->candidate_index = (int)candidate_index;
            out->used_fallback = candidate_index == 0 ? 0 : 1;
            return DEMO_OK;
        }
    }

    set_error(error, error_capacity,
              "首选及备用热点候选均与路由冲突或无法容纳 DHCP 池");
    return DEMO_ERR;
}
