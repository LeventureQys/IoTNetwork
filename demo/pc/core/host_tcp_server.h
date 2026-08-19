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

/* beta v1.1：固定一对一 TCP 服务（设计文档 8.2）。在线 + pending 总数 ≤ PROTO_HOST_MAX_CONN，
 * 第二连接回复 host_ack busy reason=single_device_only 后关闭。 */
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

private:
    struct PendingConn {
        void *sock = nullptr;
        net_addr_t peer{};
        uint64_t connect_ms = 0;
        std::vector<uint8_t> rx;
        int malformed = 0;
    };

    void HandlePending(PendingConn &pc, uint64_t now_ms);
    void HandleOnline(DeviceEntry &e, uint64_t now_ms);
    void FlushPendingTx();
    int SendFrame(void *sock, cJSON *obj);
    std::string NewSessionId();
    void RejectBusy(void *conn);
    int ActiveConnCount() const; /* 在线(conn!=null) + pending */

    net_ctx_t *net_;
    HostRegistry &reg_;
    const demo_params_t &params_;
    void *listen_ = nullptr;
    std::vector<PendingConn> pending_;
    std::map<void *, std::vector<uint8_t>> conn_rx_; /* 已注册连接收包缓冲（按句柄隔离） */
    std::mutex tx_mu_;
    std::vector<std::pair<std::string, std::string>> pending_tx_; /* (device_id, text) */
    uint32_t app_data_seq_ = 0;
};

#endif
