#include "pc_scenario.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_SCENARIO_ACTION_MAX 16

int pc_scenario_validate_backend(int backend_sim)
{
    return backend_sim ? DEMO_OK : DEMO_ERR;
}

static int parse_kind(const char *name, pc_scenario_action_kind_t *out)
{
    if (name == NULL)
        return DEMO_ERR;
    if (strcmp(name, "auto_provision") == 0) {
        *out = PC_SCENARIO_ACTION_AUTO_PROVISION;
        return DEMO_OK;
    }
    if (strcmp(name, "send_app_data") == 0) {
        *out = PC_SCENARIO_ACTION_SEND_APP_DATA;
        return DEMO_OK;
    }
    if (strcmp(name, "inject_fault") == 0) {
        *out = PC_SCENARIO_ACTION_INJECT_FAULT;
        return DEMO_OK;
    }
    if (strcmp(name, "request_stop") == 0) {
        *out = PC_SCENARIO_ACTION_REQUEST_STOP;
        return DEMO_OK;
    }
    return DEMO_ERR;
}

static int valid_after_event(const char *name)
{
    if (name == NULL)
        return 0;
    return strcmp(name, "ready") == 0 || strcmp(name, "ap_ready") == 0 ||
           strcmp(name, "session_online") == 0 || strcmp(name, "session_offline") == 0 ||
           strcmp(name, "null") == 0;
}

static int parse_action(const cJSON *item, pc_scenario_action_t *out)
{
    memset(out, 0, sizeof(*out));
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(item, "target");
    const cJSON *after = cJSON_GetObjectItemCaseSensitive(item, "after_event");
    const cJSON *delay = cJSON_GetObjectItemCaseSensitive(item, "delay_ms");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(item, "action");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(item, "args");
    if (!cJSON_IsString(id) || !cJSON_IsString(target) || !cJSON_IsString(after) ||
        !cJSON_IsNumber(delay) || !cJSON_IsString(action) || !cJSON_IsObject(args))
        return DEMO_ERR;
    if (strcmp(target->valuestring, "pc") != 0 && strcmp(target->valuestring, "device") != 0)
        return DEMO_ERR;
    if (!valid_after_event(after->valuestring))
        return DEMO_ERR;
    if (delay->valueint < 0 || delay->valueint > 60000)
        return DEMO_ERR;
    if (snprintf(out->id, sizeof(out->id), "%s", id->valuestring) >= (int)sizeof(out->id))
        return DEMO_ERR;
    snprintf(out->target, sizeof(out->target), "%s", target->valuestring);
    snprintf(out->after_event, sizeof(out->after_event), "%s", after->valuestring);
    out->delay_ms = delay->valueint;

    if (parse_kind(action->valuestring, &out->kind) != DEMO_OK)
        return DEMO_ERR;

    switch (out->kind) {
    case PC_SCENARIO_ACTION_AUTO_PROVISION: {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(args, "password");
        if (!cJSON_IsString(ssid) || !cJSON_IsString(password))
            return DEMO_ERR;
        snprintf(out->arg_ssid, sizeof(out->arg_ssid), "%s", ssid->valuestring);
        snprintf(out->arg_password, sizeof(out->arg_password), "%s", password->valuestring);
        break;
    }
    case PC_SCENARIO_ACTION_SEND_APP_DATA: {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
        if (!cJSON_IsString(text))
            return DEMO_ERR;
        snprintf(out->arg_text, sizeof(out->arg_text), "%s", text->valuestring);
        out->arg_text_len = (int)strlen(text->valuestring);
        break;
    }
    case PC_SCENARIO_ACTION_INJECT_FAULT: {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
        if (!cJSON_IsString(name))
            return DEMO_ERR;
        snprintf(out->arg_fault_name, sizeof(out->arg_fault_name), "%s", name->valuestring);
        const cJSON *json = cJSON_GetObjectItemCaseSensitive(args, "argument_json");
        if (cJSON_IsString(json))
            snprintf(out->arg_fault_json, sizeof(out->arg_fault_json), "%s", json->valuestring);
        break;
    }
    case PC_SCENARIO_ACTION_REQUEST_STOP: {
        if (cJSON_GetArraySize(args) != 0)
            return DEMO_ERR; /* args 必须为空对象 */
        break;
    }
    }
    return DEMO_OK;
}

int pc_scenario_load(const char *path, pc_scenario_t *out)
{
    if (path == NULL || path[0] == '\0' || out == NULL)
        return DEMO_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return DEMO_ERR;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return DEMO_ERR;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return DEMO_ERR_NOMEM;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL)
        return DEMO_ERR;

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(root, "actions");
    if (!cJSON_IsNumber(schema) || schema->valueint != 1 || !cJSON_IsArray(actions) ||
        cJSON_GetArraySize(actions) > PC_SCENARIO_ACTION_MAX) {
        cJSON_Delete(root);
        return DEMO_ERR;
    }

    out->schema = schema->valueint;
    int count = cJSON_GetArraySize(actions);
    for (int i = 0; i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(actions, i);
        if (parse_action(item, &out->actions[out->action_count]) != DEMO_OK) {
            cJSON_Delete(root);
            return DEMO_ERR;
        }
        /* id 文件内唯一 */
        for (int j = 0; j < out->action_count; j++)
            if (strcmp(out->actions[j].id, out->actions[out->action_count].id) == 0) {
                cJSON_Delete(root);
                return DEMO_ERR;
            }
        out->action_count++;
    }
    cJSON_Delete(root);
    return DEMO_OK;
}
