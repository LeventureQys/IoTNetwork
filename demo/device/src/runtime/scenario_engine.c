/* ============================================================================
 * scenario_engine.c - 设备侧 scenario 执行引擎（纯 C11，SS06）。
 *
 * beta v1.1 契约（设计文档第 11 节 + 阶段二 DQ）：
 * - --scenario 只允许 sim 后端（device_host_create 已强制）；
 * - action 一次一结果：target=device 的 action 恰好产生一条同 action_id 的
 *   scenario_result（status=ok|rejected|failed）；
 * - after_event 门控（ready|session_online|session_offline|null）+ delay_ms（0~60000）；
 * - send_app_data / inject_fault / request_stop。
 * - 已删除旧配网 auto_provision 与 ap_ready 门控（beta v1.1 无设备热点/配网）。
 *
 * 执行模型：单线程协作式（运行于 device runner 线程）。每个 tick 只做一次
 * 非阻塞推进，不 sleep，避免阻塞状态机。
 * ========================================================================== */
#include "scenario_engine.h"
#include "device_host_internal.h"
#include "protocol.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENARIO_MAX_ACTIONS 64

typedef enum action_kind {
    ACT_NONE = 0,
    ACT_SEND_APP_DATA,
    ACT_INJECT_FAULT,
    ACT_REQUEST_STOP
} action_kind_t;

typedef struct scenario_action {
    char id[64];
    char after_event[32];
    long delay_ms;
    action_kind_t kind;
    int target_device;
    int started;
    uint64_t trigger_at_ms;
    int result_emitted;

    char arg_text[520];
    char arg_name[64];
    char arg_argument_json[512];
} scenario_action_t;

struct scenario_engine {
    device_host_t *host;
    scenario_action_t actions[SCENARIO_MAX_ACTIONS];
    int count;
    int gate_ready;
    int gate_session_online;
    int gate_session_offline;
};

/* ---------------- 工具 ---------------- */

static void err_set(device_error_t *error, device_result_t code,
                    const char *message)
{
    if (error == NULL)
        return;
    error->code = code;
    if (message != NULL)
        snprintf(error->message, sizeof(error->message), "%s", message);
}

static const char *action_name(action_kind_t kind)
{
    switch (kind) {
    case ACT_SEND_APP_DATA:  return "send_app_data";
    case ACT_INJECT_FAULT:   return "inject_fault";
    case ACT_REQUEST_STOP:   return "request_stop";
    default:                 return "unknown";
    }
}

/* ---------------- scenario_result 输出 ---------------- */

static void emit_result(scenario_engine_t *eng, scenario_action_t *a,
                        const char *status, long request_bytes,
                        const char *reason)
{
    char data[1024];
    const char *rs = reason != NULL ? reason : "";
    snprintf(data, sizeof(data),
             "{\"action_id\":\"%s\",\"action\":\"%s\",\"status\":\"%s\","
             "\"request_bytes\":%ld,\"reason\":\"%s\"}",
             a->id, action_name(a->kind), status, request_bytes, rs);
    device_host_publish_event(eng->host, "scenario_result", "ok", 0, data);
    a->result_emitted = 1;
}

/* ---------------- 动作执行 ---------------- */

static void execute_action(scenario_engine_t *eng, scenario_action_t *a,
                           uint64_t now)
{
    (void)now;
    device_host_t *host = eng->host;
    switch (a->kind) {
    case ACT_SEND_APP_DATA: {
        size_t len = strlen(a->arg_text);
        if (len > APP_DATA_TEXT_MAX) {
            emit_result(eng, a, "rejected", (long)len, "payload_too_large");
            break;
        }
        device_error_t err;
        device_result_t rc = device_host_send_app_data(host, a->arg_text, len, &err);
        if (rc == DEVICE_OK)
            emit_result(eng, a, "ok", (long)len, "");
        else {
            char reason[64];
            snprintf(reason, sizeof(reason), "%s",
                     err.message[0] != '\0' ? err.message : "send_failed");
            emit_result(eng, a, "failed", (long)len, reason);
        }
        break;
    }

    case ACT_INJECT_FAULT: {
        device_error_t err;
        device_result_t rc = device_host_inject_fault(
            host, a->arg_name, a->arg_argument_json[0] ? a->arg_argument_json : NULL,
            &err);
        if (rc == DEVICE_OK)
            emit_result(eng, a, "ok", 0, "");
        else
            emit_result(eng, a, "failed", 0,
                        err.message[0] != '\0' ? err.message : "inject_failed");
        break;
    }

    case ACT_REQUEST_STOP: {
        device_host_request_stop(host);
        emit_result(eng, a, "ok", 0, "");
        break;
    }

    default:
        emit_result(eng, a, "failed", 0, "unknown_action");
        break;
    }
}

/* ---------------- 公开接口 ---------------- */

scenario_engine_t *scenario_engine_create(const char *scenario_path,
                                          device_host_t *host,
                                          device_error_t *error)
{
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (scenario_path == NULL || scenario_path[0] == '\0' || host == NULL) {
        err_set(error, DEVICE_ERR_INVALID_ARGUMENT, "scenario 路径或 host 为空");
        return NULL;
    }

    FILE *f = fopen(scenario_path, "rb");
    if (f == NULL) {
        err_set(error, DEVICE_ERR_CONFIG_NOT_FOUND, "scenario 文件不可读");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        err_set(error, DEVICE_ERR_CONFIG_INVALID, "scenario 文件大小异常");
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        err_set(error, DEVICE_ERR_NO_MEMORY, "scenario 读取内存不足");
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        err_set(error, DEVICE_ERR_CONFIG_INVALID, "scenario JSON 解析失败");
        return NULL;
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(root, "actions");
    if (!cJSON_IsNumber(schema) || schema->valueint != 1 || !cJSON_IsArray(actions)) {
        cJSON_Delete(root);
        err_set(error, DEVICE_ERR_CONFIG_INVALID,
                "scenario 必须为 {schema:1, actions:[...]}");
        return NULL;
    }

    scenario_engine_t *eng = (scenario_engine_t *)calloc(1, sizeof(scenario_engine_t));
    if (eng == NULL) {
        cJSON_Delete(root);
        err_set(error, DEVICE_ERR_NO_MEMORY, "scenario 引擎分配失败");
        return NULL;
    }
    eng->host = host;

    const cJSON *item;
    cJSON_ArrayForEach(item, actions) {
        if (eng->count >= SCENARIO_MAX_ACTIONS)
            break;
        if (!cJSON_IsObject(item))
            goto invalid;

        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *jt = cJSON_GetObjectItemCaseSensitive(item, "target");
        if (!cJSON_IsString(jid) || jid->valuestring[0] == '\0' ||
            strlen(jid->valuestring) >= sizeof(eng->actions[eng->count].id))
            goto invalid;
        if (!cJSON_IsString(jt))
            goto invalid;

        scenario_action_t *a = &eng->actions[eng->count];
        snprintf(a->id, sizeof(a->id), "%s", jid->valuestring);
        a->kind = ACT_NONE;
        if (strcmp(jt->valuestring, "device") == 0)
            a->target_device = 1;
        else if (strcmp(jt->valuestring, "pc") == 0)
            a->target_device = 0;
        else
            goto invalid;

        /* id 文件内唯一 */
        for (int k = 0; k < eng->count; k++) {
            if (strcmp(eng->actions[k].id, a->id) == 0)
                goto invalid;
        }

        /* target=pc：忽略（不校验业务字段，不产生结果） */
        if (!a->target_device) {
            eng->count++;
            continue;
        }

        /* after_event */
        const cJSON *jae = cJSON_GetObjectItemCaseSensitive(item, "after_event");
        if (!cJSON_IsString(jae))
            goto invalid;
        if (strcmp(jae->valuestring, "ready") == 0 ||
            strcmp(jae->valuestring, "session_online") == 0 ||
            strcmp(jae->valuestring, "session_offline") == 0 ||
            strcmp(jae->valuestring, "null") == 0) {
            snprintf(a->after_event, sizeof(a->after_event), "%s", jae->valuestring);
        } else {
            goto invalid;
        }

        /* delay_ms */
        const cJSON *jdl = cJSON_GetObjectItemCaseSensitive(item, "delay_ms");
        if (!cJSON_IsNumber(jdl) || jdl->valueint < 0 || jdl->valueint > 60000)
            goto invalid;
        a->delay_ms = jdl->valueint;

        /* action */
        const cJSON *jac = cJSON_GetObjectItemCaseSensitive(item, "action");
        if (!cJSON_IsString(jac))
            goto invalid;
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(item, "args");
        if (!cJSON_IsObject(args))
            goto invalid;

        if (strcmp(jac->valuestring, "send_app_data") == 0) {
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
            if (!cJSON_IsString(text) || strlen(text->valuestring) >= sizeof(a->arg_text))
                goto invalid;
            snprintf(a->arg_text, sizeof(a->arg_text), "%s", text->valuestring);
            a->kind = ACT_SEND_APP_DATA;
        } else if (strcmp(jac->valuestring, "inject_fault") == 0) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
            const cJSON *argj = cJSON_GetObjectItemCaseSensitive(args, "argument_json");
            if (!cJSON_IsString(name) || name->valuestring[0] == '\0' ||
                strlen(name->valuestring) >= sizeof(a->arg_name))
                goto invalid;
            snprintf(a->arg_name, sizeof(a->arg_name), "%s", name->valuestring);
            if (cJSON_IsString(argj))
                snprintf(a->arg_argument_json, sizeof(a->arg_argument_json), "%s",
                         argj->valuestring);
            a->kind = ACT_INJECT_FAULT;
        } else if (strcmp(jac->valuestring, "request_stop") == 0) {
            if (cJSON_GetArraySize(args) != 0)
                goto invalid;
            a->kind = ACT_REQUEST_STOP;
        } else {
            goto invalid;
        }

        eng->count++;
        continue;

    invalid:
        scenario_engine_destroy(eng);
        cJSON_Delete(root);
        err_set(error, DEVICE_ERR_CONFIG_INVALID, "scenario action 定义非法");
        return NULL;
    }

    cJSON_Delete(root);
    return eng;
}

void scenario_engine_destroy(scenario_engine_t *eng)
{
    if (eng != NULL)
        free(eng);
}

void scenario_engine_on_event(scenario_engine_t *eng, const char *event,
                              const char *result, const char *data_json)
{
    (void)result;
    (void)data_json;
    if (eng == NULL || event == NULL)
        return;
    if (strcmp(event, "ready") == 0)
        eng->gate_ready = 1;
    else if (strcmp(event, "session_online") == 0)
        eng->gate_session_online = 1;
    else if (strcmp(event, "session_offline") == 0)
        eng->gate_session_offline = 1;
}

void scenario_engine_tick(scenario_engine_t *eng, device_host_t *host)
{
    if (eng == NULL || host == NULL)
        return;
    uint64_t now = host->plat->monotonic_ms();
    for (int i = 0; i < eng->count; i++) {
        scenario_action_t *a = &eng->actions[i];
        if (!a->target_device || a->result_emitted)
            continue;
        if (a->after_event[0] != '\0' && a->after_event[0] != 'n') {
            int seen = 0;
            if (strcmp(a->after_event, "ready") == 0)
                seen = eng->gate_ready;
            else if (strcmp(a->after_event, "session_online") == 0)
                seen = eng->gate_session_online;
            else if (strcmp(a->after_event, "session_offline") == 0)
                seen = eng->gate_session_offline;
            if (!seen)
                continue;
        }
        if (!a->started) {
            a->started = 1;
            a->trigger_at_ms = now + (uint64_t)a->delay_ms;
            continue;
        }
        if (now < a->trigger_at_ms)
            continue;
        execute_action(eng, a, now);
    }
}
