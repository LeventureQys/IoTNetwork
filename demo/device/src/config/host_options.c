#include "device_host.h"
#include "device_path.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void err_set(device_error_t *error, device_result_t code, int platform_code,
                    const char *operation, const char *message)
{
    if (error == NULL)
        return;
    error->code = code;
    error->platform_code = platform_code;
    if (operation != NULL)
        snprintf(error->operation, sizeof(error->operation), "%s", operation);
    if (message != NULL)
        snprintf(error->message, sizeof(error->message), "%s", message);
}

static int str_ieq(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return 0;
    for (;;) {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca != cb)
            return 0;
        if (ca == '\0')
            return 1;
        a++;
        b++;
    }
}

void device_host_options_init(device_host_options_t *options)
{
    if (options == NULL)
        return;
    memset(options, 0, sizeof(*options));
    options->backend = DEVICE_BACKEND_SIM;
}

static device_result_t take_value(int argc, char *const argv[], int *i,
                                  const char *flag, const char **out,
                                  device_error_t *error)
{
    if (*i + 1 >= argc) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "parse_argv",
                "选项缺少参数值");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    (*i)++;
    *out = argv[*i];
    (void)flag;
    return DEVICE_OK;
}

device_result_t device_host_options_parse_argv(device_host_options_t *options,
                                               int argc, char *const argv[],
                                               device_error_t *error)
{
    if (options == NULL) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "parse_argv", "options 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }
    device_host_options_init(options);
    if (argv == NULL && argc > 0) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "parse_argv", "argv 为空");
        return DEVICE_ERR_INVALID_ARGUMENT;
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--config") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            options->config_path = v;
        } else if (strcmp(arg, "--backend") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            if (str_ieq(v, "sim"))
                options->backend = DEVICE_BACKEND_SIM;
            else if (str_ieq(v, "linux"))
                options->backend = DEVICE_BACKEND_LINUX;
            else if (str_ieq(v, "esp32c2"))
                options->backend = DEVICE_BACKEND_ESP32C2;
            else {
                err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "parse_argv",
                        "未知后端类型（应为 sim/linux/esp32c2）");
                return DEVICE_ERR_INVALID_ARGUMENT;
            }
        } else if (strcmp(arg, "--device-index") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            char *end = NULL;
            long idx = strtol(v, &end, 10);
            if (end == v || *end != '\0' || idx < 0 || idx > 15) {
                err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "parse_argv",
                        "设备索引必须在 0 到 15 之间");
                return DEVICE_ERR_INVALID_ARGUMENT;
            }
            options->device_index = (unsigned int)idx;
        } else if (strcmp(arg, "--fresh") == 0) {
            options->fresh = 1;
        } else if (strcmp(arg, "--duration") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            char *end = NULL;
            long d = strtol(v, &end, 10);
            if (end == v || *end != '\0' || d < 0) {
                err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "parse_argv",
                        "duration 必须是非负整数秒");
                return DEVICE_ERR_INVALID_ARGUMENT;
            }
            options->duration_seconds = (unsigned int)d;
        } else if (strcmp(arg, "--runtime-dir") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            options->runtime_dir = v;
        } else if (strcmp(arg, "--sim-catalog-dir") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            options->sim_catalog_dir = v;
        } else if (strcmp(arg, "--log-dir") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            options->log_dir = v;
        } else if (strcmp(arg, "--events-jsonl") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            options->events_jsonl_path = v;
        } else if (strcmp(arg, "--scenario") == 0) {
            const char *v = NULL;
            device_result_t rc = take_value(argc, argv, &i, arg, &v, error);
            if (rc != DEVICE_OK)
                return rc;
            options->scenario_path = v;
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "未知命令行选项：%s", arg);
            err_set(error, DEVICE_ERR_INVALID_ARGUMENT, 0, "parse_argv", msg);
            return DEVICE_ERR_INVALID_ARGUMENT;
        }
    }
    return DEVICE_OK;
}
