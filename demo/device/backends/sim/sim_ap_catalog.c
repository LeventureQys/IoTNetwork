/* ============================================================================
 * sim_ap_catalog.c - 跨进程 PC 热点 catalog（纯 C11，设备侧只读）。
 *
 * 严格实现设计文档 7.3 冻结契约（文件名、JSON 字段、校验与时效），
 * 不得另行定义 schema。无全局可变状态。
 * ========================================================================== */
#include "sim_ap_catalog.h"
#include "sim_util.h"
#include "common.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

uint64_t sim_ap_catalog_wallclock_ms(void)
{
    return sim_util_wallclock_ms();
}

unsigned long sim_ap_catalog_current_pid(void)
{
    return sim_util_current_pid();
}

int sim_ap_catalog_build_path(const char *dir, char *out, size_t out_cap)
{
    if (!dir || !out)
        return DEMO_ERR_INVAL;
    snprintf(out, out_cap, "%s%c%s", dir,
#ifdef _WIN32
             '\\',
#else
             '/',
#endif
             "pc-hotspot.json");
    return strlen(out) < out_cap ? DEMO_OK : DEMO_ERR_INVAL;
}

/* ---------------- 校验 ---------------- */

static int record_validate(const cJSON *root, sim_ap_catalog_record_t *out)
{
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *logical_gateway = cJSON_GetObjectItemCaseSensitive(root, "logical_gateway");
    const cJSON *prefix_length = cJSON_GetObjectItemCaseSensitive(root, "prefix_length");
    const cJSON *tcp_port = cJSON_GetObjectItemCaseSensitive(root, "tcp_port");
    const cJSON *loopback_host = cJSON_GetObjectItemCaseSensitive(root, "loopback_host");
    const cJSON *loopback_port = cJSON_GetObjectItemCaseSensitive(root, "loopback_port");
    const cJSON *published_at_ms = cJSON_GetObjectItemCaseSensitive(root, "published_at_ms");
    const cJSON *owner_pid = cJSON_GetObjectItemCaseSensitive(root, "owner_pid");

    if (!cJSON_IsNumber(schema) || (int)schema->valueint != SIM_AP_CATALOG_SCHEMA)
        return 0;
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0])
        return 0;
    if (!cJSON_IsString(password) || !password->valuestring[0])
        return 0;
    if (!cJSON_IsString(logical_gateway) || !cJSON_IsString(loopback_host))
        return 0;
    if (!cJSON_IsNumber(prefix_length) || prefix_length->valueint < 0 ||
        prefix_length->valueint > 32)
        return 0;
    if (!cJSON_IsNumber(tcp_port) || tcp_port->valueint <= 0 ||
        tcp_port->valueint > 65535)
        return 0;
    if (!cJSON_IsNumber(loopback_port) || loopback_port->valueint <= 0 ||
        loopback_port->valueint > 65535)
        return 0;
    if (!cJSON_IsNumber(published_at_ms) || published_at_ms->valuedouble < 0)
        return 0;
    if (!cJSON_IsNumber(owner_pid) || owner_pid->valuedouble < 0)
        return 0;

    /* IP 严格性：logical_gateway / loopback_host 必须是合法点分 IPv4 */
    {
        int ok = 0;
        sim_util_ipv4_parse(logical_gateway->valuestring, &ok);
        if (!ok)
            return 0;
        ok = 0;
        sim_util_ipv4_parse(loopback_host->valuestring, &ok);
        if (!ok)
            return 0;
    }

    if (out) {
        memset(out, 0, sizeof(*out));
        out->schema = SIM_AP_CATALOG_SCHEMA;
        sim_util_copy_bounded(out->ssid, sizeof(out->ssid), ssid->valuestring);
        sim_util_copy_bounded(out->password, sizeof(out->password), password->valuestring);
        sim_util_copy_bounded(out->logical_gateway, sizeof(out->logical_gateway),
                              logical_gateway->valuestring);
        sim_util_copy_bounded(out->loopback_host, sizeof(out->loopback_host),
                              loopback_host->valuestring);
        out->prefix_length = prefix_length->valueint;
        out->tcp_port = (unsigned int)tcp_port->valueint;
        out->loopback_port = (unsigned int)loopback_port->valueint;
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
    if (!root)
        return -1;
    int valid = record_validate(root, out);
    cJSON_Delete(root);
    return valid ? 0 : -1;
}

int sim_ap_catalog_read(const char *dir, sim_ap_catalog_record_t *out, uint64_t now_ms)
{
    if (!dir || !out)
        return DEMO_ERR_INVAL;
    char path[1024];
    if (sim_ap_catalog_build_path(dir, path, sizeof(path)) != DEMO_OK)
        return DEMO_ERR_INVAL;

    sim_ap_catalog_record_t rec;
    if (catalog_read_file(path, &rec) != 0)
        return DEMO_ERR;

    if (now_ms == 0)
        now_ms = sim_util_wallclock_ms();
    if (now_ms > rec.published_at_ms + SIM_AP_CATALOG_TTL_MS &&
        !sim_util_pid_alive(rec.owner_pid))
        return DEMO_ERR; /* 过期且 owner 进程不存在 → 忽略 */

    *out = rec;
    return DEMO_OK;
}
