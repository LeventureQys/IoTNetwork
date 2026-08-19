#ifndef DEMO_HOST_ANNOUNCER_H
#define DEMO_HOST_ANNOUNCER_H

#include <cstdint>
#include "net_abstraction.h"
#include "params.h"

class HostAnnouncer {
public:
    HostAnnouncer(net_ctx_t *net, const demo_params_t &params);
    ~HostAnnouncer();
    int Start();
    void Stop();
    void Poll(uint64_t now_ms); /* 每 discovery_normal_interval_ms 广播一次 announce */
    void SendBye();
    uint64_t last_seq() const { return seq_; }

private:
    net_ctx_t *net_;
    const demo_params_t &params_;
    void *sock_ = nullptr;
    uint64_t seq_ = 0;
    uint64_t last_send_ms_ = 0;
};

#endif
