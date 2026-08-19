#include "linux_nvs.h"

#include "cJSON.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TAG "LINUX_NVS"
#define LINUX_NVS_DEFAULT_PATH "run/device_linux.nvs.json"
#define LINUX_NVS_MAX_FILE_SIZE (1024U * 1024U)

static const char k_base64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct linux_nvs {
    char path[384];
};

static int base64_encode(const uint8_t *data, size_t length, char *out,
                         size_t capacity)
{
    size_t output_length = ((length + 2) / 3) * 4;
    size_t i;
    size_t o = 0;

    if (output_length + 1 > capacity)
        return DEMO_ERR;
    for (i = 0; i < length; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < length)
            v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < length)
            v |= (uint32_t)data[i + 2];
        out[o++] = k_base64[(v >> 18) & 0x3F];
        out[o++] = k_base64[(v >> 12) & 0x3F];
        out[o++] = i + 1 < length ? k_base64[(v >> 6) & 0x3F] : '=';
        out[o++] = i + 2 < length ? k_base64[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return DEMO_OK;
}

static int base64_char_value(char c)
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

static int base64_decode(const char *in, uint8_t *out, size_t capacity,
                         size_t *out_length)
{
    uint32_t buffer = 0;
    int bits = 0;
    size_t written = 0;

    if (in == NULL)
        return DEMO_ERR;
    while (*in != '\0') {
        int value;
        if (*in == '=' || *in == '\n' || *in == '\r' || *in == ' ') {
            ++in;
            continue;
        }
        value = base64_char_value(*in);
        if (value < 0)
            return DEMO_ERR;
        buffer = (buffer << 6) | (uint32_t)value;
        bits += 6;
        ++in;
        if (bits >= 8) {
            bits -= 8;
            if (written >= capacity)
                return DEMO_ERR;
            out[written++] = (uint8_t)((buffer >> bits) & 0xFF);
        }
    }
    *out_length = written;
    return DEMO_OK;
}

static cJSON *load_root(linux_nvs_t *nvs)
{
    FILE *file;
    long file_size;
    char *buffer;
    cJSON *root;

    file = fopen(nvs->path, "rb");
    if (file == NULL) {
        if (errno == ENOENT)
            return cJSON_CreateObject();
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    if ((unsigned long)file_size > LINUX_NVS_MAX_FILE_SIZE) {
        fclose(file);
        return NULL;
    }
    buffer = (char *)malloc((size_t)file_size + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);
    buffer[file_size] = '\0';
    root = cJSON_ParseWithLengthOpts(buffer, (size_t)file_size + 1, NULL, 1);
    free(buffer);
    if (root == NULL)
        return NULL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static int save_root(linux_nvs_t *nvs, const cJSON *root)
{
    char *serialized;
    char tmp_path[384];
    int tmp_fd;
    FILE *file;
    size_t length;
    size_t written;
    int result = DEMO_ERR;
    const char *slash;

    serialized = cJSON_PrintUnformatted(root);
    if (serialized == NULL)
        return DEMO_ERR_NOMEM;
    length = strlen(serialized);
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-%ld", nvs->path,
                 (long)getpid()) >= (int)sizeof(tmp_path)) {
        free(serialized);
        return DEMO_ERR_INVAL;
    }
    slash = strrchr(nvs->path, '/');
    if (slash != NULL && slash != nvs->path) {
        char dir[256];
        size_t dir_length = (size_t)(slash - nvs->path);
        if (dir_length < sizeof(dir)) {
            memcpy(dir, nvs->path, dir_length);
            dir[dir_length] = '\0';
            if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
                free(serialized);
                return DEMO_ERR;
            }
        }
    }
    tmp_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (tmp_fd < 0) {
        free(serialized);
        return DEMO_ERR;
    }
    file = fdopen(tmp_fd, "w");
    if (file == NULL) {
        close(tmp_fd);
        unlink(tmp_path);
        free(serialized);
        return DEMO_ERR;
    }
    written = fwrite(serialized, 1, length, file);
    if (written == length && fflush(file) == 0 && fsync(tmp_fd) == 0)
        result = DEMO_OK;
    if (fclose(file) != 0)
        result = DEMO_ERR;
    if (result == DEMO_OK && rename(tmp_path, nvs->path) != 0)
        result = DEMO_ERR;
    if (result != DEMO_OK)
        unlink(tmp_path);
    free(serialized);
    return result;
}

int linux_nvs_create(linux_nvs_t **out, const char *path)
{
    linux_nvs_t *nvs;

    if (out == NULL)
        return DEMO_ERR_INVAL;
    *out = NULL;
    nvs = (linux_nvs_t *)calloc(1, sizeof(*nvs));
    if (nvs == NULL)
        return DEMO_ERR_NOMEM;
    if (path == NULL || path[0] == '\0')
        snprintf(nvs->path, sizeof(nvs->path), "%s", LINUX_NVS_DEFAULT_PATH);
    else if (strlen(path) >= sizeof(nvs->path)) {
        free(nvs);
        return DEMO_ERR_INVAL;
    } else
        snprintf(nvs->path, sizeof(nvs->path), "%s", path);
    *out = nvs;
    return DEMO_OK;
}

void linux_nvs_destroy(linux_nvs_t *nvs)
{
    if (nvs == NULL)
        return;
    memset(nvs, 0, sizeof(*nvs));
    free(nvs);
}

int linux_nvs_get(linux_nvs_t *nvs, const char *key, uint8_t *buf, int *len)
{
    cJSON *root;
    const cJSON *item;
    int result;

    if (nvs == NULL || key == NULL || buf == NULL || len == NULL || *len <= 0)
        return DEMO_ERR_INVAL;
    root = load_root(nvs);
    if (root == NULL) {
        LOG_E(TAG, "NVS 文件损坏或不可读: %s", nvs->path);
        return DEMO_ERR;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL) {
        *len = 0;
        cJSON_Delete(root);
        return DEMO_ERR;
    }
    {
        size_t decoded = 0;
        result = base64_decode(item->valuestring, buf, (size_t)*len, &decoded);
        *len = (int)decoded;
    }
    cJSON_Delete(root);
    return result;
}

int linux_nvs_set(linux_nvs_t *nvs, const char *key, const uint8_t *buf, int len)
{
    cJSON *root;
    char *encoded;
    size_t encoded_capacity;
    int result;

    if (nvs == NULL || key == NULL || buf == NULL || len < 0)
        return DEMO_ERR_INVAL;
    encoded_capacity = ((size_t)len + 2) / 3 * 4 + 1;
    encoded = (char *)malloc(encoded_capacity);
    if (encoded == NULL)
        return DEMO_ERR_NOMEM;
    if (base64_encode(buf, (size_t)len, encoded, encoded_capacity) != DEMO_OK) {
        free(encoded);
        return DEMO_ERR_INVAL;
    }
    root = load_root(nvs);
    if (root == NULL) {
        free(encoded);
        return DEMO_ERR;
    }
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (item != NULL)
            cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    }
    {
        cJSON *item = cJSON_AddStringToObject(root, key, encoded);
        if (item == NULL) {
            cJSON_Delete(root);
            free(encoded);
            return DEMO_ERR_NOMEM;
        }
    }
    result = save_root(nvs, root);
    cJSON_Delete(root);
    free(encoded);
    return result;
}

int linux_nvs_erase(linux_nvs_t *nvs, const char *key)
{
    cJSON *root;
    int existed;
    int result;

    if (nvs == NULL || key == NULL)
        return DEMO_ERR_INVAL;
    root = load_root(nvs);
    if (root == NULL)
        return DEMO_ERR;
    existed = cJSON_GetObjectItemCaseSensitive(root, key) != NULL;
    if (!existed) {
        cJSON_Delete(root);
        return DEMO_OK;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    result = save_root(nvs, root);
    cJSON_Delete(root);
    return result;
}
