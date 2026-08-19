#ifndef DEMO_HOST_MDNS_H
#define DEMO_HOST_MDNS_H

#include "net_abstraction.h"
#include "params.h"

class HostMdns {
public:
    HostMdns(net_ctx_t *net, const demo_params_t &params);
    int Register(); /* DEMO_OK=注册成功；失败仅记录警告，不影响组播主路径 */
    void Unregister();

private:
    net_ctx_t *net_;
    const demo_params_t &params_;
};

#endif
