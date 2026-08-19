#include "host_mdns.h"
#include "cJSON.h"
#include "protocol.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

HostMdns::HostMdns(net_ctx_t *net, const demo_params_t &params)
    : net_(net), params_(params)
{
}

int HostMdns::Register()
{
    net_mdns_service_t svc;
    memset(&svc, 0, sizeof(svc));
    snprintf(svc.instance, sizeof(svc.instance), "%s", PROTO_MDNS_INSTANCE);
    snprintf(svc.type, sizeof(svc.type), "%s", PROTO_MDNS_TYPE);
    unsigned a, b, c, d;
    if (sscanf(params_.host_virtual_ip, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        svc.addr.ip = (uint32_t)((d << 24) | (c << 16) | (b << 8) | a);
    }
    svc.addr.port = (uint16_t)(((uint16_t)params_.host_tcp_port << 8) |
                               ((uint16_t)params_.host_tcp_port >> 8));
    snprintf(svc.txt, sizeof(svc.txt), "ip=%s;tcp_port=%d", params_.host_virtual_ip,
             params_.host_tcp_port);
    if (net_mdns_register(net_, &svc) == DEMO_OK) {
        LOG_I("HOST", "mDNS 服务已注册：%s.local（兼容注册，真实发现以 UDP 组播为主）",
              PROTO_MDNS_TYPE);
        return DEMO_OK;
    }
    LOG_W("HOST", "mDNS 服务注册失败");
    return DEMO_ERR;
}

void HostMdns::Unregister()
{
    net_mdns_unregister(net_, PROTO_MDNS_TYPE);
}
