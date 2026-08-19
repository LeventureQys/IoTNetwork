#include "device_events.h"
#include "device_platform.h"
#include "common.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct device_events {
    FILE *file;
    uint64_t seq;
    const device_platform_t *plat;
};

device_events_t *device_events_open(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return NULL;
    FILE *f = fopen(path, "ab");
    if (f == NULL)
        return NULL;
    device_events_t *e = (device_events_t *)calloc(1, sizeof(device_events_t));
    if (e == NULL) {
        fclose(f);
        return NULL;
    }
    e->file = f;
    e->plat = &device_platform_default;
    return e;
}

void device_events_close(device_events_t *e)
{
    if (e == NULL)
        return;
    if (e->file != NULL) {
        fflush(e->file);
        fclose(e->file);
        e->file = NULL;
    }
    free(e);
}

int device_events_write(device_events_t *e, const char *event, const char *result,
                        int code, const char *data_json)
{
    if (e == NULL || e->file == NULL || event == NULL || result == NULL)
        return DEMO_ERR_INVAL;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return DEMO_ERR_NOMEM;
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddNumberToObject(root, "seq", (double)(++e->seq));
    cJSON_AddNumberToObject(root, "time_ms", (double)e->plat->monotonic_ms());
    cJSON_AddStringToObject(root, "role", "device");
    cJSON_AddStringToObject(root, "device_id", "");
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddStringToObject(root, "result", result);
    cJSON_AddNumberToObject(root, "code", code);
    if (data_json != NULL && data_json[0] != '\0') {
        cJSON *data = cJSON_Parse(data_json);
        if (data != NULL) {
            cJSON_AddItemToObject(root, "data", data);
        } else {
            cJSON *empty = cJSON_CreateObject();
            cJSON_AddItemToObject(root, "data", empty);
        }
    } else {
        cJSON_AddItemToObject(root, "data", cJSON_CreateObject());
    }
    char *line = cJSON_PrintUnformatted(root);
    int rc = DEMO_OK;
    if (line == NULL) {
        rc = DEMO_ERR_NOMEM;
    } else {
        if (fprintf(e->file, "%s\n", line) < 0 || fflush(e->file) != 0)
            rc = DEMO_ERR;
        free(line);
    }
    cJSON_Delete(root);
    return rc;
}
