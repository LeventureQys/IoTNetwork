/* ============================================================================
 * sim_ap_catalog.c - 跨进程模拟 AP catalog（纯 C11）。
 *
 * 严格实现设计文档第 8.5 节冻结契约（文件名、JSON 字段、原子发布、
 * 删除与时效），不得另行定义 schema。无全局可变状态。
 * ========================================================================== */
#include "sim_ap_catalog.h"
#include "sim_util.h"
#include "common.h"
#include "protocol.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#endif

unsigned long sim_ap_catalog_current_pid(void)
{
    return sim_util_current_pid();
}

uint64_t sim_ap_catalog_wallclock_ms(void)
{
    return sim_util_wallclock_ms();
}

void sim_ap_catalog_build_device_id(unsigned int device_index, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    snprintf(out, cap, "02:00:00:00:00:%02X", (unsigned)(device_index + 1));
}

int sim_ap_catalog_build_path(const char *dir, unsigned int device_index,
                              char *out, size_t out_cap)
{
    if (!dir || !out)
        return DEMO_ERR_INVAL;
    snprintf(out, out_cap, "%s%cdevice-%u.json", dir,
#ifdef _WIN32
             '\\',
#else
             '/',
#endif
             device_index);
    return strlen(out) < out_cap ? DEMO_OK : DEMO_ERR_INVAL;
}

/* ---------------- 发布 / 删除 ---------------- */

int sim_ap_catalog_publish(const char *dir, const sim_ap_catalog_record_t *record)
{
    if (!dir || !record)
        return DEMO_ERR_INVAL;

    char path[1024];
    if (sim_ap_catalog_build_path(dir, record->device_index, path, sizeof(path)) != DEMO_OK)
        return DEMO_ERR_INVAL;

    cJSON *root = cJSON_CreateObject();
    if (!root)
        return DEMO_ERR_NOMEM;
    cJSON_AddNumberToObject(root, "schema", SIM_AP_CATALOG_SCHEMA);
    cJSON_AddNumberToObject(root, "device_index", (double)record->device_index);
    cJSON_AddStringToObject(root, "device_id", record->device_id);
    cJSON_AddStringToObject(root, "ssid", record->ssid);
    cJSON_AddStringToObject(root, "bssid", record->bssid);
    cJSON_AddStringToObject(root, "logical_ip", record->logical_ip);
    cJSON_AddStringToObject(root, "loopback_host", record->loopback_host);
    cJSON_AddNumberToObject(root, "provision_port", (double)record->provision_port);
    cJSON_AddNumberToObject(root, "published_at_ms", (double)record->published_at_ms);
    cJSON_AddNumberToObject(root, "owner_pid", (double)record->owner_pid);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text)
        return DEMO_ERR_NOMEM;

    int rc = sim_util_atomic_write_file(path, text, strlen(text));
    free(text);
    return rc == 0 ? DEMO_OK : DEMO_ERR;
}

int sim_ap_catalog_remove(const char *dir, unsigned int device_index, unsigned long pid)
{
    if (!dir)
        return DEMO_ERR_INVAL;
    char path[1024];
    if (sim_ap_catalog_build_path(dir, device_index, path, sizeof(path)) != DEMO_OK)
        return DEMO_ERR_INVAL;
    remove(path);
    /* 清理本进程全部残留 tmp（<name>.tmp-<pid>-*，写失败/并发发布可能遗留） */
    {
        const char *base = strrchr(path, '\\');
        const char *base2 = strrchr(path, '/');
        if (!base || (base2 && base2 > base))
            base = base2;
        if (base)
            base++;
        else
            base = path;
        char prefix[1200];
        snprintf(prefix, sizeof(prefix), "%s.tmp-%lu", base, pid);
        size_t prefix_len = strlen(prefix);
#ifdef _WIN32
        {
            char pattern[1216];
            snprintf(pattern, sizeof(pattern), "%s*", prefix);
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pattern, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    char victim[1300];
                    snprintf(victim, sizeof(victim), "%s\\%s", dir, fd.cFileName);
                    if (strncmp(fd.cFileName, prefix, prefix_len) == 0)
                        remove(victim);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
#else
        {
            DIR *d = opendir(dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (strncmp(e->d_name, prefix, prefix_len) == 0) {
                        char victim[1300];
                        snprintf(victim, sizeof(victim), "%s/%s", dir, e->d_name);
                        remove(victim);
                    }
                }
                closedir(d);
            }
        }
#endif
    }
    return DEMO_OK;
}

/* ---------------- 读取 ---------------- */

static int record_validate(const cJSON *root, sim_ap_catalog_record_t *out)
{
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *device_index = cJSON_GetObjectItemCaseSensitive(root, "device_index");
    const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *bssid = cJSON_GetObjectItemCaseSensitive(root, "bssid");
    const cJSON *logical_ip = cJSON_GetObjectItemCaseSensitive(root, "logical_ip");
    const cJSON *loopback_host = cJSON_GetObjectItemCaseSensitive(root, "loopback_host");
    const cJSON *provision_port = cJSON_GetObjectItemCaseSensitive(root, "provision_port");
    const cJSON *published_at_ms = cJSON_GetObjectItemCaseSensitive(root, "published_at_ms");
    const cJSON *owner_pid = cJSON_GetObjectItemCaseSensitive(root, "owner_pid");

    if (!cJSON_IsNumber(schema) || (int)schema->valueint != SIM_AP_CATALOG_SCHEMA)
        return 0;
    if (!cJSON_IsNumber(device_index) || device_index->valueint < 0 ||
        device_index->valueint > SIM_AP_CATALOG_MAX_INDEX)
        return 0;
    if (!cJSON_IsString(device_id) || !cJSON_IsString(ssid) || !cJSON_IsString(bssid) ||
        !cJSON_IsString(logical_ip) || !cJSON_IsString(loopback_host))
        return 0;
    if (!cJSON_IsNumber(provision_port) || provision_port->valueint <= 0 ||
        provision_port->valueint > 65535)
        return 0;
    if (!cJSON_IsNumber(published_at_ms) || published_at_ms->valuedouble < 0)
        return 0;
    if (!cJSON_IsNumber(owner_pid) || owner_pid->valuedouble < 0)
        return 0;

    /* IP 严格性：logical_ip / loopback_host 必须是合法点分 IPv4 */
    {
        int ok = 0;
        sim_util_ipv4_parse(logical_ip->valuestring, &ok);
        if (!ok)
            return 0;
        ok = 0;
        sim_util_ipv4_parse(loopback_host->valuestring, &ok);
        if (!ok)
            return 0;
    }
    /* 字段非空 */
    if (!ssid->valuestring[0] || !device_id->valuestring[0] || !bssid->valuestring[0])
        return 0;

    if (out) {
        memset(out, 0, sizeof(*out));
        out->schema = SIM_AP_CATALOG_SCHEMA;
        out->device_index = (unsigned int)device_index->valueint;
        sim_util_copy_bounded(out->device_id, sizeof(out->device_id), device_id->valuestring);
        sim_util_copy_bounded(out->ssid, sizeof(out->ssid), ssid->valuestring);
        sim_util_copy_bounded(out->bssid, sizeof(out->bssid), bssid->valuestring);
        sim_util_copy_bounded(out->logical_ip, sizeof(out->logical_ip), logical_ip->valuestring);
        sim_util_copy_bounded(out->loopback_host, sizeof(out->loopback_host), loopback_host->valuestring);
        out->provision_port = (unsigned int)provision_port->valueint;
        out->published_at_ms = (uint64_t)published_at_ms->valuedouble;
        out->owner_pid = (unsigned long)owner_pid->valuedouble;
    }
    return 1;
}

/* 读取正式文件内容并校验；返回 0=有效（可填充 out），非 0=无效/缺失 */
static int catalog_read_file(const char *path, sim_ap_catalog_record_t *out)
{
    char *text = (char *)malloc(SIM_AP_CATALOG_MAX_FILE + 1);
    if (!text)
        return -1;
    size_t rd = 0;
    if (sim_util_read_file(path, text, SIM_AP_CATALOG_MAX_FILE + 1, &rd) != 0) {
        free(text);
        return -1;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        cJSON_Delete(root);
        return -1;
    }
    int valid = record_validate(root, out);
    cJSON_Delete(root);
    return valid ? 0 : -1;
}

int sim_ap_catalog_read(const char *dir, unsigned int device_index,
                        sim_ap_catalog_record_t *out)
{
    if (!dir || !out)
        return DEMO_ERR_INVAL;
    char path[1024];
    if (sim_ap_catalog_build_path(dir, device_index, path, sizeof(path)) != DEMO_OK)
        return DEMO_ERR_INVAL;
    return catalog_read_file(path, out) == 0 ? DEMO_OK : DEMO_ERR;
}

int sim_ap_catalog_list(const char *dir, sim_ap_catalog_record_t *out, int capacity,
                        int *out_count, uint64_t now_ms)
{
    if (!dir || !out_count)
        return DEMO_ERR_INVAL;
    *out_count = 0;
    if (capacity < 0 || (capacity > 0 && !out))
        return DEMO_ERR_INVAL;
    if (now_ms == 0)
        now_ms = sim_util_wallclock_ms();

    char pattern[1100];
    int count = 0;
#ifdef _WIN32
    snprintf(pattern, sizeof(pattern), "%s\\*.json", dir);
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return DEMO_OK; /* 目录不存在/无匹配 → 0 条 */
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (strstr(fd.cFileName, ".tmp-") != NULL)
                continue; /* 只读正式文件，跳过 tmp */
            size_t len = strlen(fd.cFileName);
            if (len < 6 || strcmp(fd.cFileName + len - 5, ".json") != 0)
                continue;
            char full[1200];
            snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
            sim_ap_catalog_record_t rec;
            if (catalog_read_file(full, &rec) != 0)
                continue; /* 损坏/校验失败 → 忽略 */
            if (now_ms > rec.published_at_ms + SIM_AP_CATALOG_TTL_MS &&
                !sim_util_pid_alive(rec.owner_pid))
                continue; /* 过期且 owner 进程不存在 → 忽略 */
            if (count < capacity && out)
                out[count] = rec;
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    (void)pattern;
    {
        DIR *d = opendir(dir);
        if (!d)
            return DEMO_OK;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR)
                continue;
            if (strstr(e->d_name, ".tmp-") != NULL)
                continue;
            size_t len = strlen(e->d_name);
            if (len < 6 || strcmp(e->d_name + len - 5, ".json") != 0)
                continue;
            char full[1200];
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            sim_ap_catalog_record_t rec;
            if (catalog_read_file(full, &rec) != 0)
                continue;
            if (now_ms > rec.published_at_ms + SIM_AP_CATALOG_TTL_MS &&
                !sim_util_pid_alive(rec.owner_pid))
                continue;
            if (count < capacity && out)
                out[count] = rec;
            count++;
        }
        closedir(d);
    }
#endif
    *out_count = count;
    return DEMO_OK;
}

int sim_ap_catalog_find_ssid(const char *dir, const char *ssid,
                             sim_ap_catalog_record_t *out, uint64_t now_ms)
{
    if (!ssid || !out)
        return DEMO_ERR_INVAL;
    sim_ap_catalog_record_t records[16];
    int count = 0;
    int rc = sim_ap_catalog_list(dir, records, 16, &count, now_ms);
    if (rc != DEMO_OK)
        return rc;
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(records[i].ssid, ssid) == 0) {
            *out = records[i];
            return DEMO_OK;
        }
    }
    return DEMO_ERR;
}
