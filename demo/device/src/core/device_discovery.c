#include "device_discovery.h"
#include "device_app.h"
#include "device_eventlog.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int discovery_start(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);
    app->discovery_round = 0;
    app->discovery_fast_until_ms = now + (uint64_t)app->params->discovery_fast_window_ms;
    app->last_announce_seq = 0;
    app->last_mcast_poll_ms = 0;

    if (app->mcast_sock == NULL) {
        int rc = net_udp_mcast_join(app->net, app->params->mcast_group,
                                    (uint16_t)app->params->mcast_port, &app->mcast_sock);
        if (rc != DEMO_OK)
            LOG_W(app->device_id, "服务发现：加入组播失败，将回退到候选地址");
        else
            LOG_I(app->device_id, "服务发现：UDP 组播监听已启动，地址=%s:%d",
                  app->params->mcast_group, app->params->mcast_port);
    }
    return DEMO_OK;
}

/* host_announce / host_bye 处理（UDP 报文，无帧前缀） */
static void discovery_handle_udp(device_app_t *app, cJSON *json)
{
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
    if (!cmd || !cJSON_IsString(cmd))
        return;
    const char *name = cmd->valuestring;

    if (strcmp(name, CMD_HOST_BYE) == 0) {
        evlog_record(app, "已收到上位机离线通知 host_bye");
        LOG_I(app->device_id, "服务发现：收到 host_bye，切换候选地址并快速轮询");
        app->discovery_round = 2;               /* 候选直连优先 */
        app->discovery_fast_until_ms =
            net_time_ms(app->net) + (uint64_t)app->params->discovery_fast_window_ms;
        return;
    }

    if (strcmp(name, CMD_HOST_ANNOUNCE) == 0) {
        const cJSON *proto = cJSON_GetObjectItemCaseSensitive(json, "proto");
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(json, "seq");
        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(json, "ip");
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(json, "tcp_port");
        if (!cJSON_IsNumber(proto) || proto->valueint != PROTO_VERSION) {
            LOG_D(app->device_id, "服务发现：host_announce 协议版本不匹配，已忽略");
            return;
        }
        if (!cJSON_IsNumber(seq) || seq->valueint <= (int)app->last_announce_seq) {
            LOG_D(app->device_id, "服务发现：host_announce 序号过期，已忽略");
            return;
        }
        if (!cJSON_IsString(ip) || !cJSON_IsNumber(port))
            return;
        app->last_announce_seq = (uint64_t)seq->valueint;
        unsigned a, b, c, d;
        if (sscanf(ip->valuestring, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            app->sess_host.ip = (uint32_t)((d << 24) | (c << 16) | (b << 8) | a);
            app->sess_host.port = dev_htons((uint16_t)port->valueint);
            LOG_I(app->device_id, "服务发现：host_announce 指向 %s:%d", ip->valuestring,
                  port->valueint);
        }
    }
}

int discovery_poll(device_app_t *app)
{
    uint64_t now = net_time_ms(app->net);

    if (app->discovery_round == 0) {
        /* 一级：mDNS */
        net_mdns_service_t svc;
        memset(&svc, 0, sizeof(svc));
        int rc = net_mdns_resolve(app->net, PROTO_MDNS_TYPE, &svc, 2000);
        if (rc == DEMO_OK && svc.addr.port != 0) {
            app->sess_host = svc.addr;
            evlog_record(app, "通过 mDNS 发现上位机");
            return 1;
        }
        app->discovery_round = 1;
        return 0;
    }

    if (app->discovery_round == 1) {
        /* 二级：组播监听（快速窗口节流） */
        int interval = (now < app->discovery_fast_until_ms)
            ? app->params->discovery_fast_interval_ms
            : app->params->discovery_normal_interval_ms;
        if (now - app->last_mcast_poll_ms < (uint64_t)interval)
            return 0;
        app->last_mcast_poll_ms = now;

        if (app->mcast_sock == NULL) {
            app->discovery_round = 2;
            return 0;
        }
        uint8_t buf[1024];
        int n = net_udp_recv(app->net, app->mcast_sock, buf, (int)sizeof(buf), NULL);
        if (n == DEMO_ERR_AGAIN) {
            /* 超时降级候选（状态驻留判定） */
            if (now - app->state_enter_ms >=
                (uint64_t)app->params->discovery_candidate_timeout_ms) {
                app->discovery_round = 2;
            }
            return 0;
        }
        if (n <= 0)
            return 0;
        buf[n] = '\0';
        cJSON *json = cJSON_Parse((const char *)buf);
        if (json) {
            discovery_handle_udp(app, json);
            cJSON_Delete(json);
        }
        if (app->sess_host.port != 0)
            return 1;
        return 0;
    }

    /* 三级：候选列表直连探测 */
    if (app->candidate_count <= 0) {
        app->discovery_round = 1;
        return 0;
    }
    for (int i = 0; i < app->candidate_count; i++) {
        void *probe = NULL;
        int rc = net_tcp_connect(app->net, &app->candidate_list[i], &probe, 500);
        if (rc == DEMO_OK) {
            net_sock_close(app->net, probe);
            app->sess_host = app->candidate_list[i];
            evlog_record(app, "host found via candidate list");
            return 1;
        }
    }
    /* 全部失败：回组播 + 退避（状态驻留计时） */
    app->discovery_round = 1;
    return 0;
}

void discovery_stop(device_app_t *app)
{
    if (app->mcast_sock) {
        net_sock_close(app->net, app->mcast_sock);
        app->mcast_sock = NULL;
    }
}
