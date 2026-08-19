/* ============================================================================
 * scenario_engine.c - 设备侧 scenario 执行引擎（纯 C11，SS06）。
 *
 * 契约（设计文档第 11 节）：
 * - --scenario 只允许 sim 后端（device_host_create 已强制）；
 * - action 一次一结果：target=device 的 action 恰好产生一条同 action_id 的
 *   scenario_result（status=ok|rejected|failed）；
 * - after_event 门控（ready|ap_ready|session_online|session_offline|null）+
 *   delay_ms（0~60000）；
 * - auto_provision / send_app_data / inject_fault / request_stop。
 *
 * 执行模型：单线程协作式（运行于 device runner 线程）。每个 tick 只做一次
 * 非阻塞推进（连接/收包轮询），不 sleep，避免阻塞状态机；auto_provision
 * 是阶段性状态机（连接→auth→wifi_config），各阶段带绝对截止时间。
 * ========================================================================== */
#include "scenario_engine.h"
#include "device_host_internal.h"
#include "frame.h"
#include "protocol.h"
#include "common.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENARIO_MAX_ACTIONS 64
#define SCENARIO_PHASE_TIMEOUT_MS 10000u

typedef enum action_kind {
    ACT_NONE = 0,
    ACT_AUTO_PROVISION,
    ACT_SEND_APP_DATA,
    ACT_INJECT_FAULT,
    ACT_REQUEST_STOP
} action_kind_t;

typedef enum auto_phase {
    APHASE_IDLE = 0,      /* 等待连接 */
    APHASE_AUTH_SENT,     /* 已发 auth，等 auth_result */
    APHASE_WIFI_SENT,     /* 已发 wifi_config，等 wifi_result */
    APHASE_DONE
} auto_phase_t;

typedef struct scenario_action {
    char id[64];
    char after_event[32];
    long delay_ms;
    action_kind_t kind;
    int target_device;
    int gate_seen;
    int started;
    uint64_t trigger_at_ms;
    int result_emitted;

    char arg_ssid[33];
    char arg_password[64];
    char arg_text[520];
    char arg_name[64];
    char arg_argument_json[512];

    /* auto_provision 客户端状态 */
    auto_phase_t phase;
    void *cli_sock;
    uint64_t phase_deadline_ms;
    uint8_t rx_buf[1024];
    int rx_len;
} scenario_action_t;

struct scenario_engine {
    device_host_t *host;
    scenario_action_t actions[SCENARIO_MAX_ACTIONS];
    int count;
    int gate_ready;
    int gate_ap_ready;
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

static uint16_t h_to_net16(uint16_t v)
{
    union { uint16_t u16; uint8_t bytes[2]; } probe;
    probe.u16 = 1;
    if (probe.bytes[0] == 1)
        return (uint16_t)((v << 8) | (v >> 8));
    return v;
}

static uint32_t ipv4_net(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
    uint32_t value = 0;
    uint8_t *bytes = (uint8_t *)&value;
    bytes[0] = b0;
    bytes[1] = b1;
    bytes[2] = b2;
    bytes[3] = b3;
    return value;
}

static const char *action_name(action_kind_t kind)
{
    switch (kind) {
    case ACT_AUTO_PROVISION: return "auto_provision";
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

/* ---------------- 帧发送/收包轮询（auto_provision 客户端） ---------------- */

static int client_send_json(scenario_engine_t *eng, scenario_action_t *a,
                            cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (s == NULL)
        return DEMO_ERR_NOMEM;
    uint8_t frame[PROTO_MSG_MAX_LEN + PROTO_FRAME_HEAD_LEN];
    int flen = frame_wrap((const uint8_t *)s, (int)strlen(s), frame,
                          (int)sizeof(frame));
    free(s);
    if (flen < 0)
        return DEMO_ERR;
    int n = net_sock_send(eng->host->net, a->cli_sock, frame, flen);
    /* 与设备侧发送语义一致：n>=0 视为整帧写出成功 */
    return n >= 0 ? DEMO_OK : n;
}

/* 轮询收包；返回 1=收到并已按 cmd 处理，0=无数据，-1=错误/超时 */
static int client_poll_frame(scenario_engine_t *eng, scenario_action_t *a)
{
    uint8_t buf[512];
    int n = net_sock_recv(eng->host->net, a->cli_sock, buf, (int)sizeof(buf));
    if (n == DEMO_ERR_AGAIN)
        return 0;
    if (n < 0)
        return -1;
    if (a->rx_len + n > (int)sizeof(a->rx_buf))
        return -1;
    memcpy(a->rx_buf + a->rx_len, buf, (size_t)n);
    a->rx_len += n;

    int off = 0, len = 0, consumed = 0;
    int rc = frame_parse(a->rx_buf, a->rx_len, &off, &len, &consumed);
    if (rc == 0)
        return 0; /* 需要更多数据 */
    if (rc == DEMO_ERR)
        return -1;
    a->rx_buf[off + len] = '\0';
    cJSON *json = cJSON_Parse((const char *)(a->rx_buf + off));
    /* 消费该帧 */
    memmove(a->rx_buf, a->rx_buf + consumed, (size_t)(a->rx_len - consumed));
    a->rx_len -= consumed;
    if (json == NULL)
        return -1;
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
    const char *name = (cmd && cJSON_IsString(cmd)) ? cmd->valuestring : NULL;
    int handled = 0;
    if (name != NULL) {
        if (strcmp(name, CMD_AUTH_RESULT) == 0) {
            const cJSON *st = cJSON_GetObjectItemCaseSensitive(json, "status");
            const char *status = (st && cJSON_IsString(st)) ? st->valuestring : "fail";
            if (strcmp(status, "ok") == 0)
                handled = 2; /* auth ok：调用方应发 wifi_config */
            else
                handled = -2; /* auth fail */
        } else if (strcmp(name, CMD_WIFI_RESULT) == 0) {
            const cJSON *st = cJSON_GetObjectItemCaseSensitive(json, "status");
            const char *status = (st && cJSON_IsString(st)) ? st->valuestring : "fail";
            handled = (strcmp(status, "ok") == 0) ? 3 : -3;
        } else {
            handled = 0; /* 忽略其他帧，继续等 */
        }
    }
    cJSON_Delete(json);
    return handled;
}

/* ---------------- auto_provision 客户端（连接本机配网服务） ---------------- */

static void auto_provision_tick(scenario_engine_t *eng, scenario_action_t *a,
                                uint64_t now)
{
    device_host_t *host = eng->host;

    if (a->phase == APHASE_IDLE) {
        if (a->cli_sock == NULL) {
            net_addr_t addr;
            addr.ip = ipv4_net(127, 0, 0, 1);
            addr.port = h_to_net16((uint16_t)host->provision_port);
            if (net_tcp_connect(host->net, &addr, &a->cli_sock, 1000) != DEMO_OK) {
                a->cli_sock = NULL;
                if (now >= a->phase_deadline_ms) {
                    emit_result(eng, a, "failed", 0, "connect_failed");
                    return;
                }
                return; /* 下轮重试 */
            }
            a->phase_deadline_ms = now + SCENARIO_PHASE_TIMEOUT_MS;
        }
        /* 已连接：发 auth（PIN 与配网服务固定验证码一致） */
        cJSON *auth = cJSON_CreateObject();
        cJSON_AddStringToObject(auth, "cmd", CMD_AUTH);
        cJSON_AddStringToObject(auth, "pin", "5935");
        int rc = client_send_json(eng, a, auth);
        cJSON_Delete(auth);
        if (rc != DEMO_OK) {
            net_sock_close(host->net, a->cli_sock);
            a->cli_sock = NULL;
            emit_result(eng, a, "failed", 0, "auth_send_failed");
            return;
        }
        a->phase = APHASE_AUTH_SENT;
        a->phase_deadline_ms = now + SCENARIO_PHASE_TIMEOUT_MS;
        return;
    }

    if (a->phase == APHASE_AUTH_SENT || a->phase == APHASE_WIFI_SENT) {
        int rc = client_poll_frame(eng, a);
        if (rc == 0) {
            if (now >= a->phase_deadline_ms) {
                net_sock_close(host->net, a->cli_sock);
                a->cli_sock = NULL;
                emit_result(eng, a, "failed", 0,
                            a->phase == APHASE_AUTH_SENT ? "auth_timeout"
                                                         : "wifi_timeout");
            }
            return;
        }
        if (rc == 2) {
            /* auth ok → wifi_config */
            cJSON *cfg = cJSON_CreateObject();
            cJSON_AddStringToObject(cfg, "cmd", CMD_WIFI_CONFIG);
            cJSON_AddStringToObject(cfg, "ssid", a->arg_ssid);
            cJSON_AddStringToObject(cfg, "password", a->arg_password);
            int snd = client_send_json(eng, a, cfg);
            cJSON_Delete(cfg);
            if (snd != DEMO_OK) {
                net_sock_close(host->net, a->cli_sock);
                a->cli_sock = NULL;
                emit_result(eng, a, "failed", 0, "wifi_config_send_failed");
                return;
            }
            a->phase = APHASE_WIFI_SENT;
            a->phase_deadline_ms = now + SCENARIO_PHASE_TIMEOUT_MS;
            return;
        }
        if (rc == 3) {
            net_sock_close(host->net, a->cli_sock);
            a->cli_sock = NULL;
            emit_result(eng, a, "ok", 0, "");
            return;
        }
        if (rc == -2 || rc == -3) {
            net_sock_close(host->net, a->cli_sock);
            a->cli_sock = NULL;
            emit_result(eng, a, "failed", 0,
                        rc == -2 ? "auth_rejected" : "wifi_rejected");
            return;
        }
        /* -1 或其他：连接/协议错误 */
        net_sock_close(host->net, a->cli_sock);
        a->cli_sock = NULL;
        emit_result(eng, a, "failed", 0, "protocol_error");
        return;
    }
}

/* ---------------- 动作执行 ---------------- */

static void execute_action(scenario_engine_t *eng, scenario_action_t *a,
                           uint64_t now)
{
    device_host_t *host = eng->host;
    switch (a->kind) {
    case ACT_AUTO_PROVISION:
        if (a->phase == APHASE_IDLE)
            a->phase_deadline_ms = now + SCENARIO_PHASE_TIMEOUT_MS;
        auto_provision_tick(eng, a, now);
        break;

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
            strcmp(jae->valuestring, "ap_ready") == 0 ||
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

        if (strcmp(jac->valuestring, "auto_provision") == 0) {
            const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
            const cJSON *pass = cJSON_GetObjectItemCaseSensitive(args, "password");
            if (!cJSON_IsString(ssid) || !cJSON_IsString(pass) ||
                ssid->valuestring[0] == '\0' || pass->valuestring[0] == '\0' ||
                strlen(ssid->valuestring) >= sizeof(a->arg_ssid) ||
                strlen(pass->valuestring) >= sizeof(a->arg_password))
                goto invalid;
            snprintf(a->arg_ssid, sizeof(a->arg_ssid), "%s", ssid->valuestring);
            snprintf(a->arg_password, sizeof(a->arg_password), "%s", pass->valuestring);
            a->kind = ACT_AUTO_PROVISION;
        } else if (strcmp(jac->valuestring, "send_app_data") == 0) {
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
    if (eng == NULL)
        return;
    for (int i = 0; i < eng->count; i++) {
        if (eng->actions[i].cli_sock != NULL) {
            net_sock_close(eng->host->net, eng->actions[i].cli_sock);
            eng->actions[i].cli_sock = NULL;
        }
    }
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
    else if (strcmp(event, "ap_ready") == 0)
        eng->gate_ap_ready = 1;
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
            else if (strcmp(a->after_event, "ap_ready") == 0)
                seen = eng->gate_ap_ready;
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
