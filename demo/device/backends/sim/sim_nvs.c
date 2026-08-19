/* ============================================================================
 * sim_nvs.c - JSON blob 键值存储（纯 C11，实例化）。
 *
 * 迁移自旧 net_sim/sim_backend.cpp 的 NVS 部分 + base64 工具，收紧两处
 * 语义（非法 base64 失败、缓冲不足报长度不截断），其余逐字等价。
 * ========================================================================== */
#include "sim_nvs.h"
#include "sim_util.h"
#include "common.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

#define SIM_NVS_FILE_CAP (64 * 1024)

struct sim_nvs {
    char file_path[1024];
};

static const char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

int sim_nvs_base64_encode(const uint8_t *data, size_t len, char *out, size_t out_cap, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!data && len > 0)
        return DEMO_ERR;
    if (!out)
        return DEMO_ERR;
    size_t need = ((len + 2) / 3) * 4 + 1; /* 含结尾 NUL */
    if (out_len)
        *out_len = need - 1;
    if (need > out_cap)
        return DEMO_ERR;
    size_t o = 0;
    size_t i;
    for (i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len)
            v |= (uint32_t)data[i + 2];
        out[o++] = kBase64Alphabet[(v >> 18) & 0x3F];
        out[o++] = kBase64Alphabet[(v >> 12) & 0x3F];
        out[o++] = i + 1 < len ? kBase64Alphabet[(v >> 6) & 0x3F] : '=';
        out[o++] = i + 2 < len ? kBase64Alphabet[v & 0x3F] : '=';
    }
    out[o] = 0;
    return DEMO_OK;
}

int sim_nvs_base64_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!in)
        return DEMO_ERR;
    size_t len = strlen(in);
    if (len == 0)
        return DEMO_OK; /* 空字符串 → 0 字节（合法） */

    /* 去掉 \r \n（文件可能含换行） */
    size_t cleaned = 0;
    char *buf = (char *)malloc(len + 1);
    if (!buf)
        return DEMO_ERR;
    size_t i;
    for (i = 0; i < len; i++) {
        if (in[i] != '\r' && in[i] != '\n')
            buf[cleaned++] = in[i];
    }
    buf[cleaned] = 0;

    if (cleaned == 0) {
        free(buf);
        return DEMO_OK;
    }
    if (cleaned % 4 != 0) {
        free(buf);
        return DEMO_ERR; /* 长度非法 */
    }

    /* 校验字符集与 padding 规则 */
    size_t pad = 0;
    for (i = 0; i < cleaned; i++) {
        char c = buf[i];
        if (c == '=') {
            /* '=' 只允许出现在最后（剩余字符 <= 2），组内位置由下方分组校验兜底 */
            if (cleaned - i > 2) {
                free(buf);
                return DEMO_ERR;
            }
            pad++;
        } else if (base64_value(c) < 0) {
            free(buf);
            return DEMO_ERR; /* 非法字符 */
        }
    }
    if (pad > 2) {
        free(buf);
        return DEMO_ERR;
    }
    /* 组内 padding 位置合法性：'=' 不能在组的前两个字符 */
    for (i = 0; i < cleaned; i += 4) {
        if (buf[i] == '=' || (i + 1 < cleaned && buf[i + 1] == '=')) {
            free(buf);
            return DEMO_ERR;
        }
        if (buf[i + 2] == '=' && buf[i + 3] != '=') {
            free(buf);
            return DEMO_ERR;
        }
    }

    size_t need = cleaned / 4 * 3;
    if (cleaned >= 2 && buf[cleaned - 2] == '=')
        need -= 2;
    else if (cleaned >= 1 && buf[cleaned - 1] == '=')
        need -= 1;
    if (out_len)
        *out_len = need;
    if (need > out_cap) {
        free(buf);
        return DEMO_ERR; /* 缓冲不足：报所需长度，不截断 */
    }

    size_t o = 0;
    for (i = 0; i < cleaned; i += 4) {
        uint32_t v = 0;
        v |= (uint32_t)base64_value(buf[i]) << 18;
        if (buf[i + 1] != '=')
            v |= (uint32_t)base64_value(buf[i + 1]) << 12;
        if (buf[i + 2] != '=')
            v |= (uint32_t)base64_value(buf[i + 2]) << 6;
        if (buf[i + 3] != '=')
            v |= (uint32_t)base64_value(buf[i + 3]);
        out[o++] = (uint8_t)((v >> 16) & 0xFF);
        if (buf[i + 2] != '=' && o < need)
            out[o++] = (uint8_t)((v >> 8) & 0xFF);
        if (buf[i + 3] != '=' && o < need)
            out[o++] = (uint8_t)(v & 0xFF);
    }
    free(buf);
    return DEMO_OK;
}

sim_nvs_t *sim_nvs_open(const char *file_path)
{
    if (!file_path || !file_path[0])
        return NULL;
    sim_nvs_t *nvs = (sim_nvs_t *)calloc(1, sizeof(sim_nvs_t));
    if (!nvs)
        return NULL;
    sim_util_copy_bounded(nvs->file_path, sizeof(nvs->file_path), file_path);
    return nvs;
}

void sim_nvs_close(sim_nvs_t *nvs)
{
    free(nvs);
}

/* 读取现有文件；缺失/损坏返回 NULL（调用方按空对象处理）。 */
static cJSON *nvs_load(const char *path)
{
    char *text = (char *)malloc(SIM_NVS_FILE_CAP + 1);
    if (!text)
        return NULL;
    size_t rd = 0;
    if (sim_util_read_file(path, text, SIM_NVS_FILE_CAP + 1, &rd) != 0) {
        free(text);
        return NULL;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static int nvs_save(const char *path, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    if (!out)
        return DEMO_ERR;
    int rc = sim_util_atomic_write_file(path, out, strlen(out));
    free(out);
    return rc == 0 ? DEMO_OK : DEMO_ERR;
}

int sim_nvs_get(const sim_nvs_t *nvs, const char *key, uint8_t *buf, int cap, int *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!nvs || !key || !out_len)
        return DEMO_ERR_INVAL;
    if (cap < 0 || (buf == NULL && cap > 0))
        return DEMO_ERR_INVAL;

    cJSON *root = nvs_load(nvs->file_path);
    if (!root) {
        *out_len = 0;
        return DEMO_ERR; /* 文件缺失/损坏 → 键不存在 */
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item)) {
        cJSON_Delete(root);
        *out_len = 0;
        return DEMO_ERR;
    }
    size_t decoded_len = 0;
    int rc = sim_nvs_base64_decode(item->valuestring, buf, (size_t)cap, &decoded_len);
    cJSON_Delete(root);
    if (rc != DEMO_OK) {
        /* 非法 base64：decoded_len=0；缓冲不足：decoded_len=所需长度（不截断） */
        *out_len = (int)decoded_len;
        return DEMO_ERR;
    }
    *out_len = (int)decoded_len;
    return DEMO_OK;
}

int sim_nvs_set(sim_nvs_t *nvs, const char *key, const uint8_t *buf, int len)
{
    if (!nvs || !key || (buf == NULL && len > 0) || len < 0)
        return DEMO_ERR_INVAL;

    cJSON *root = nvs_load(nvs->file_path);
    if (!root)
        root = cJSON_CreateObject();
    if (!root)
        return DEMO_ERR_NOMEM;

    char b64[4096];
    size_t b64_len = 0;
    int rc = sim_nvs_base64_encode(buf, (size_t)len, b64, sizeof(b64), &b64_len);
    if (rc != DEMO_OK) {
        cJSON_Delete(root);
        return rc;
    }
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(root, key);
    if (existing)
        cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddStringToObject(root, key, b64);

    int save_rc = nvs_save(nvs->file_path, root);
    cJSON_Delete(root);
    return save_rc;
}

int sim_nvs_erase(sim_nvs_t *nvs, const char *key)
{
    if (!nvs || !key)
        return DEMO_ERR_INVAL;
    cJSON *root = nvs_load(nvs->file_path);
    if (!root)
        return DEMO_OK; /* 无文件/损坏 → 无需擦除（旧实现语义） */
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    int save_rc = nvs_save(nvs->file_path, root);
    cJSON_Delete(root);
    return save_rc;
}
