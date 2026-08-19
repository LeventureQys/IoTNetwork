#include "host_announcer.h"
#include "cJSON.h"
#include "protocol.h"
#include "log.h"
#include "pc_event.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

HostAnnouncer::HostAnnouncer(net_ctx_t *net, const demo_params_t &params)
    : net_(net), params_(params)
{
}

HostAnnouncer::~HostAnnouncer()
{
    Stop();
}

int HostAnnouncer::Start()
{
    int rc = net_udp_mcast_join(net_, params_.mcast_group, (uint16_t)params_.mcast_port, &sock_);
    if (rc != DEMO_OK) {
        LOG_E("HOST", "加入组播失败：%s:%d", params_.mcast_group, params_.mcast_port);
        return rc;
    }
    seq_ = 1;
    LOG_I("HOST", "组播通告器已启动：%s:%d", params_.mcast_group, params_.mcast_port);
    return DEMO_OK;
}

void HostAnnouncer::Stop()
{
    if (sock_) {
        net_sock_close(net_, sock_);
        sock_ = nullptr;
    }
}

void HostAnnouncer::Poll(uint64_t now_ms)
{
    if (!sock_)
        return;
    if (last_send_ms_ != 0 &&
        now_ms - last_send_ms_ < (uint64_t)params_.discovery_normal_interval_ms)
        return;
    last_send_ms_ = now_ms;

    cJSON *ann = cJSON_CreateObject();
    cJSON_AddStringToObject(ann, "cmd", CMD_HOST_ANNOUNCE);
    cJSON_AddNumberToObject(ann, "proto", PROTO_VERSION);
    cJSON_AddNumberToObject(ann, "seq", (int)seq_++);
    cJSON_AddStringToObject(ann, "ip", params_.host_virtual_ip);
    cJSON_AddNumberToObject(ann, "tcp_port", params_.host_tcp_port);
    char *s = cJSON_PrintUnformatted(ann);
    if (s) {
        net_udp_send(net_, params_.mcast_group, (uint16_t)params_.mcast_port,
                     (const uint8_t *)s, (int)strlen(s));
        LOG_I("HOST", "已发送 host_announce：ip=%s tcp_port=%d 序号=%d",
              params_.host_virtual_ip, params_.host_tcp_port, (int)(seq_ - 1));
        free(s);
    }
    cJSON_Delete(ann);
}

void HostAnnouncer::SendBye()
{
    if (!sock_)
        return;
    cJSON *bye = cJSON_CreateObject();
    cJSON_AddStringToObject(bye, "cmd", CMD_HOST_BYE);
    cJSON_AddNumberToObject(bye, "seq", (int)seq_++);
    char *s = cJSON_PrintUnformatted(bye);
    if (s) {
        int send_result = net_udp_send(net_, params_.mcast_group, (uint16_t)params_.mcast_port,
                                       (const uint8_t *)s, (int)strlen(s));
        LOG_I("HOST", "已发送 host_bye，序号=%d", (int)(seq_ - 1));
        char data[64];
        snprintf(data, sizeof(data), "{\"send_result\":%d}", send_result);
        pc_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = "host_bye_sent";
        ev.result = send_result >= 0 ? "ok" : "fail";
        ev.code = send_result;
        ev.data_json = data;
        pc_events_emit(&ev);
        free(s);
    }
    cJSON_Delete(bye);
}
