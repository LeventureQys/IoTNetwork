#include "device_provision.h"
#include "device_app.h"
#include "device_eventlog.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 内部 ---------------- */

static int device_send(device_app_t *app, void *sock, cJSON *obj)
{
    return device_send_frame(app, sock, obj);
}

static int prov_reply(device_app_t *app, cJSON *reply)
{
    if (app->ap_conn)
        return device_send(app, app->ap_conn, reply);
    return DEMO_ERR;
}

/* ---------------- 启动 ---------------- */

int prov_server_start(device_app_t *app)
{
    /* 固定 PIN（5935，上下位机同一验证码） */
    snprintf(app->ap_pin, sizeof(app->ap_pin), "5935");
    /* 固定 WPA2 密码（上下位机同一密码，供 pc 端配网连接使用） */
    snprintf(app->ap_password, sizeof(app->ap_password), "modutech_leventure");

    int dev_idx = app->dev_index;
    uint16_t real_port = app->params->use_real_wifi_sta
        ? (uint16_t)PROTO_TCP_PORT
        : (uint16_t)(app->params->device_ap_port_base + dev_idx);

    /* 先监听配网 TCP 端口，再启动热点：避免热点就绪后客户端立即连接而服务未就绪 */
    int rc = net_tcp_listen(app->net, real_port, &app->ap_listen);
    if (rc != DEMO_OK)
        return rc;
    rc = net_wifi_ap_start(app->net, app->ap_ssid, app->ap_password, app->ap_pin);
    if (rc != DEMO_OK) {
        net_sock_close(app->net, app->ap_listen);
        app->ap_listen = NULL;
        return rc;
    }
    app->ap_pin_fail_count = 0;
    app->provision_auth_ok = 0;
    app->provision_wifi_ok = 0;
    app->provision_handoff_deadline_ms = 0;
    app->ap_conn = NULL;
    app->ap_conn_start_ms = 0;

    /* 结构化事件：配网 AP 已可连接（endpoint 为实际绑定地址；sim 后端为
     * loopback，真实 Linux 后端为热点网关地址，由后端/后续版本填充） */
    {
        char data[192];
        if (app->params->use_real_wifi_sta)
            snprintf(data, sizeof(data), "{\"ssid\":\"%s\",\"endpoint\":\"<ap-gateway>:%u\"}",
                     app->ap_ssid, (unsigned)real_port);
        else
            snprintf(data, sizeof(data), "{\"ssid\":\"%s\",\"endpoint\":\"127.0.0.1:%u\"}",
                     app->ap_ssid, (unsigned)real_port);
        device_app_publish_event(app, "ap_ready", "ok", 0, data);
    }

    LOG_I(app->device_id, "设备热点已启动，SSID=%s，PIN=%s（设备标签）",
          app->ap_ssid, app->ap_pin);
    evlog_record(app, "设备热点已启动：%s", app->ap_ssid);
    return DEMO_OK;
}

/* ---------------- 轮询 ---------------- */

int prov_server_poll(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    int active = PROV_RET_OK;

    if (app->ap_listen == NULL)
        return PROV_RET_OK;

    /* accept 新配网客户端（一次仅一个） */
    void *conn = NULL;
    int rc = net_tcp_accept(app->net, app->ap_listen, &conn, NULL);
    if (rc == DEMO_OK) {
        if (app->ap_conn) {
            net_sock_close(app->net, app->ap_conn);
            LOG_I(app->device_id, "配网：新客户端已替换旧客户端");
        }
        app->ap_conn = conn;
        app->ap_conn_start_ms = now;
        app->provision_auth_ok = 0;
        app->rx_len = 0; /* 新连接重置帧缓冲 */
        active = PROV_RET_ACTIVE;
    }

    /* 已连接客户端收包 */
    if (app->ap_conn) {
        device_app_handle_rx(app, app->ap_conn, 0);
        /* 超时判定 */
        if (!app->provision_auth_ok &&
            now - app->ap_conn_start_ms >= (uint64_t)app->params->provision_auth_timeout_ms) {
            LOG_W(app->device_id, "配网：等待 auth 超时，断开连接");
            net_sock_close(app->net, app->ap_conn);
            app->ap_conn = NULL;
            active = PROV_RET_ACTIVE;
        } else if (!app->provision_wifi_ok && app->provision_auth_ok &&
                   now - app->ap_conn_start_ms >=
                       (uint64_t)app->params->provision_auth_timeout_ms +
                           (uint64_t)app->params->provision_wifi_cfg_timeout_ms) {
            LOG_W(app->device_id, "配网：等待 wifi_config 超时，断开连接");
            net_sock_close(app->net, app->ap_conn);
            app->ap_conn = NULL;
            active = PROV_RET_ACTIVE;
        }
    }
    return active;
}

/* ---------------- 消息处理 ---------------- */

/* 结构化事件：auth_result 收发完成（配网侧回复即触发） */
static void prov_publish_auth_result(device_app_t *app, const char *status)
{
    char data[64];
    snprintf(data, sizeof(data), "{\"status\":\"%s\"}", status);
    device_app_publish_event(app, "auth_result", "ok", 0, data);
}

/* 结构化事件：wifi_result 收发完成；reason 为数字错误码（无连接尝试时为 -1） */
static void prov_publish_wifi_result(device_app_t *app, const char *status,
                                     int reason)
{
    char data[64];
    snprintf(data, sizeof(data), "{\"status\":\"%s\",\"reason\":%d}", status, reason);
    device_app_publish_event(app, "wifi_result", "ok", 0, data);
}

void prov_server_on_msg(device_app_t *app, cJSON *msg)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(msg, "cmd");
    if (!cmd || !cJSON_IsString(cmd))
        return;
    const char *name = cmd->valuestring;

    if (strcmp(name, CMD_AUTH) == 0) {
        if (app->provision_auth_ok) {
            /* 已认证再收 auth：畸形（由 handle_rx 计数，此处仅日志） */
            LOG_W(app->device_id, "配网：收到重复的 auth");
            return;
        }
        const cJSON *pin = cJSON_GetObjectItemCaseSensitive(msg, "pin");
        if (!cJSON_IsString(pin) || strlen(pin->valuestring) != 4) {
            app->err_auth_fails++;
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "cmd", CMD_AUTH_RESULT);
            cJSON_AddStringToObject(reply, "status", "fail");
            prov_reply(app, reply);
            cJSON_Delete(reply);
            prov_publish_auth_result(app, "fail");
            if (app->ap_conn) {
                net_sock_close(app->net, app->ap_conn);
                app->ap_conn = NULL;
            }
            return;
        }
        if (strcmp(pin->valuestring, app->ap_pin) == 0) {
            app->provision_auth_ok = 1;
            app->ap_pin[0] = '\0'; /* PIN 作废（防重放） */
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "cmd", CMD_AUTH_RESULT);
            cJSON_AddStringToObject(reply, "status", "ok");
            prov_reply(app, reply);
            cJSON_Delete(reply);
            prov_publish_auth_result(app, "ok");
            LOG_I(app->device_id, "配网：auth 成功，PIN 已作废");
        } else {
            app->ap_pin_fail_count++;
            app->err_auth_fails++;
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "cmd", CMD_AUTH_RESULT);
            cJSON_AddStringToObject(reply, "status", "fail");
            prov_reply(app, reply);
            cJSON_Delete(reply);
            prov_publish_auth_result(app, "fail");
            LOG_W(app->device_id, "配网：auth 失败（%d/%d）", app->ap_pin_fail_count,
                  app->params->provision_pin_fail_max);
            if (app->ap_conn) {
                net_sock_close(app->net, app->ap_conn);
                app->ap_conn = NULL;
            }
        }
        return;
    }

    if (strcmp(name, CMD_WIFI_CONFIG) == 0) {
        if (!app->provision_auth_ok) {
            LOG_W(app->device_id, "配网：尚未通过 auth 就收到 wifi_config");
            return;
        }
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(msg, "ssid");
        const cJSON *pass = cJSON_GetObjectItemCaseSensitive(msg, "password");
        if (!cJSON_IsString(ssid) || !cJSON_IsString(pass) ||
            strlen(ssid->valuestring) == 0 || strlen(ssid->valuestring) > 32 ||
            strlen(pass->valuestring) < 8 || strlen(pass->valuestring) > 63) {
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "cmd", CMD_WIFI_RESULT);
            cJSON_AddStringToObject(reply, "status", "fail");
            cJSON_AddStringToObject(reply, "reason", "输入无效");
            prov_reply(app, reply);
            cJSON_Delete(reply);
            prov_publish_wifi_result(app, "fail", -1);
            LOG_W(app->device_id, "配网：wifi_config 参数无效");
            return;
        }

        /* 尝试连接目标 WiFi（reason 分类 + 重试） */
        wifi_reason_t reason = WIFI_REASON_OK;
        int rc = net_wifi_sta_connect(app->net, ssid->valuestring, pass->valuestring, &reason);
        int tries = 1;
        while (rc != DEMO_OK && (reason == WIFI_REASON_NO_AP_FOUND ||
                                 reason == WIFI_REASON_HANDSHAKE_TIMEOUT) &&
               tries < app->params->provision_sta_try_max) {
            device_sleep_ms(500);
            rc = net_wifi_sta_connect(app->net, ssid->valuestring, pass->valuestring, &reason);
            tries++;
        }

        if (rc != DEMO_OK) {
            const char *why = "超时";
            if (reason == WIFI_REASON_AUTH_FAIL) why = "密码错误";
            else if (reason == WIFI_REASON_NO_AP_FOUND) why = "信号太弱";
            else if (reason == WIFI_REASON_5G_BAND) why = "目标为5G网络";
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "cmd", CMD_WIFI_RESULT);
            cJSON_AddStringToObject(reply, "status", "fail");
            cJSON_AddStringToObject(reply, "reason", why);
            prov_reply(app, reply);
            cJSON_Delete(reply);
            prov_publish_wifi_result(app, "fail", (int)reason);
            evlog_record(app, "配网失败：%s", why);
            return;
        }

        uint32_t ip = 0;
        char actual_ssid[33] = {0};
        net_wifi_get_ip(app->net, &ip);
        if (ip == 0 ||
            net_wifi_get_current_ssid(app->net, actual_ssid, (int)sizeof(actual_ssid)) != DEMO_OK ||
            strcmp(actual_ssid, ssid->valuestring) != 0) {
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "cmd", CMD_WIFI_RESULT);
            cJSON_AddStringToObject(reply, "status", "fail");
            cJSON_AddStringToObject(reply, "reason", ip == 0 ? "未获取IP" : "实际连接SSID不一致");
            prov_reply(app, reply);
            cJSON_Delete(reply);
            prov_publish_wifi_result(app, "fail", (int)reason);
            LOG_W(app->device_id, "配网校验失败：期望SSID=%s，实际SSID=%s，IP=%u",
                  ssid->valuestring, actual_ssid, (unsigned)ip);
            evlog_record(app, "配网失败：目标SSID连接校验未通过");
            net_wifi_sta_disconnect(app->net);
            return;
        }

        /* 连接及实际 SSID 校验成功后才写 NVS（未确认），新组在前，旧组保留为备选 */
        char new_ssid[33], new_pass[64];
        snprintf(new_ssid, sizeof(new_ssid), "%s", ssid->valuestring);
        snprintf(new_pass, sizeof(new_pass), "%s", pass->valuestring);
        /* 相同凭据也移动到第 0 位，确保后续确认和启动都使用本次目标。 */
        int existing_index = -1;
        for (int i = 0; i < app->cred_count; i++) {
            if (strcmp(app->creds[i], new_ssid) == 0 && strcmp(app->cred_pass[i], new_pass) == 0) {
                existing_index = i;
                break;
            }
        }
        if (existing_index >= 0) {
            for (int i = existing_index; i > 0; i--) {
                snprintf(app->creds[i], sizeof(app->creds[i]), "%s", app->creds[i - 1]);
                snprintf(app->cred_pass[i], sizeof(app->cred_pass[i]), "%s", app->cred_pass[i - 1]);
                app->cred_confirmed[i] = app->cred_confirmed[i - 1];
            }
        } else {
            int upper = app->cred_count < PROTO_WIFI_CRED_MAX
                ? app->cred_count : PROTO_WIFI_CRED_MAX - 1;
            for (int i = upper; i > 0; i--) {
                snprintf(app->creds[i], sizeof(app->creds[i]), "%s", app->creds[i - 1]);
                snprintf(app->cred_pass[i], sizeof(app->cred_pass[i]), "%s", app->cred_pass[i - 1]);
                app->cred_confirmed[i] = app->cred_confirmed[i - 1];
            }
            if (app->cred_count < PROTO_WIFI_CRED_MAX)
                app->cred_count++;
        }
        snprintf(app->creds[0], sizeof(app->creds[0]), "%s", new_ssid);
        snprintf(app->cred_pass[0], sizeof(app->cred_pass[0]), "%s", new_pass);
        app->cred_confirmed[0] = 0;
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "schema", 1);
        cJSON *arr = cJSON_AddArrayToObject(root, "creds");
        for (int i = 0; i < app->cred_count; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "ssid", app->creds[i]);
            cJSON_AddStringToObject(it, "password", app->cred_pass[i]);
            cJSON_AddNumberToObject(it, "confirmed", app->cred_confirmed[i]);
            cJSON_AddItemToArray(arr, it);
        }
        int nvs_rc = DEMO_ERR;
        {
            char *s = cJSON_PrintUnformatted(root);
            if (s) {
                nvs_rc = net_nvs_set(app->net, "wifi_creds", (const uint8_t *)s, (int)strlen(s));
                free(s);
            }
        }
        cJSON_Delete(root);
        if (nvs_rc != DEMO_OK) {
            device_creds_reload(app);
            cJSON *reply = cJSON_CreateObject();
            cJSON_AddStringToObject(reply, "cmd", CMD_WIFI_RESULT);
            cJSON_AddStringToObject(reply, "status", "fail");
            cJSON_AddStringToObject(reply, "reason", "WiFi配置保存失败");
            prov_reply(app, reply);
            cJSON_Delete(reply);
            prov_publish_wifi_result(app, "fail", -1);
            evlog_record(app, "配网失败：WiFi配置保存失败");
            net_wifi_sta_disconnect(app->net);
            return;
        }
        app->cred_active = 0;
        app->provision_confirm_deadline_ms = net_time_ms(app->net) +
            (uint64_t)app->params->provision_confirm_window_ms;
        app->provision_wifi_ok = 1; /* 配网完成标记：wifi_config 成功后才允许进发现阶段 */
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "cmd", CMD_WIFI_RESULT);
        cJSON_AddStringToObject(reply, "status", "ok");
        char ipbuf[32];
        snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF,
                 (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        cJSON_AddStringToObject(reply, "device_ip", ipbuf);
        int send_rc = prov_reply(app, reply);
        cJSON_Delete(reply);
        if (send_rc != DEMO_OK)
            LOG_W(app->device_id, "配网：wifi_result 发送失败，返回码=%d", send_rc);
        prov_publish_wifi_result(app, "ok", WIFI_REASON_OK);
        {
            uint64_t grace = (uint64_t)(app->params->provision_handoff_grace_ms > 0
                ? app->params->provision_handoff_grace_ms : 0);
            app->provision_handoff_deadline_ms = net_time_ms(app->net) + grace;
        }
        LOG_I(app->device_id,
              "配网：目标 WiFi 配置成功，等待 close_ap 或 %d 毫秒后自动交接",
              app->params->provision_handoff_grace_ms);
        evlog_record(app, "配网成功：%s", new_ssid);
        return;
    }

    if (strcmp(name, CMD_CLOSE_AP) == 0) {
        LOG_I(app->device_id, "配网：已收到 close_ap");
        app->provision_handoff_deadline_ms = 0;
        prov_server_stop(app);
        return;
    }

    /* 未知命令：由 handle_rx 按协议文档 2.6 处理（此处不处理） */
    LOG_D(app->device_id, "配网：未处理的命令 %s", name);
}

/* ---------------- 连接关闭 ---------------- */

void prov_server_on_conn_closed(device_app_t *app)
{
    if (app->ap_conn) {
        net_sock_close(app->net, app->ap_conn);
        app->ap_conn = NULL;
    }
    /* 认证失败 5 次 → AP 锁定：关 AP + 退避（状态机转 BOOT 等待） */
    if (app->ap_pin_fail_count >= app->params->provision_pin_fail_max &&
        app->ap_listen != NULL) {
        LOG_W(app->device_id, "配网：PIN 连续失败，热点已锁定");
        evlog_record(app, "配网 PIN 已锁定");
        prov_server_stop(app);
        app->boot_backoff_until =
            net_time_ms(app->net) + (uint64_t)app->params->provision_ap_backoff_ms;
    }
}

/* ---------------- 收尾 ---------------- */

void prov_server_stop(device_app_t *app)
{
    app->provision_handoff_deadline_ms = 0;
    if (app->ap_conn) {
        net_sock_close(app->net, app->ap_conn);
        app->ap_conn = NULL;
    }
    if (app->ap_listen) {
        net_sock_close(app->net, app->ap_listen);
        app->ap_listen = NULL;
    }
    if (app->ap_ssid[0] != '\0' && app->ap_pin_fail_count >= app->params->provision_pin_fail_max) {
        /* PIN 锁定路径也关 AP */
    }
    net_wifi_ap_stop(app->net);
    LOG_I(app->device_id, "配网：设备热点已停止");
}
