#ifndef DEMO_HOST_TCP_SERVER_H
#define DEMO_HOST_TCP_SERVER_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "cJSON.h"
#include "net_abstraction.h"
#include "params.h"
#include "host_registry.h"

class HostTcpServer {
public:
    HostTcpServer(net_ctx_t *net, HostRegistry &reg, const demo_params_t &params);
    ~HostTcpServer();
    int Start(); /* 幂等：已有 listener 时返回 DEMO_OK */
    void Stop(); /* 幂等：关闭 pending/online 连接与 listener；可重复调用 */
    bool Started() const { return listen_ != nullptr; }
    void Poll(uint64_t now_ms);
    void BroadcastCloseAll();
    /* UI 线程调用：入队一条联调消息，由 Poll 在 host 线程冲刷发送。
     * 返回 DEMO_OK=已入队；DEMO_ERR=id/text 为空或 text 超 APP_DATA_TEXT_MAX */
    int QueueAppData(const std::string &device_id, const std::string &text);
    void SetBusyOverride(int limit); /* <0 恢复 */
    int BusyOverride() const { return busy_override_; }

private:
    struct PendingConn {
        void *sock = nullptr;
        uint64_t connect_ms = 0;
        std::vector<uint8_t> rx;
        int malformed = 0;
    };

    void HandlePending(PendingConn &pc, uint64_t now_ms);
    void HandleOnline(DeviceEntry &e, uint64_t now_ms);
    void FlushPendingTx();
    int SendFrame(void *sock, cJSON *obj);
    std::string NewSessionId();

    net_ctx_t *net_;
    HostRegistry &reg_;
    const demo_params_t &params_;
    void *listen_ = nullptr;
    int busy_override_ = -1;
    std::vector<PendingConn> pending_;
    std::map<void *, std::vector<uint8_t>> conn_rx_; /* 已注册连接收包缓冲（按句柄隔离） */
    std::mutex tx_mu_;
    std::vector<std::pair<std::string, std::string>> pending_tx_; /* (device_id, text) */
    uint32_t app_data_seq_ = 0;
};

#endif
