#ifndef DEMO_HOST_TCP_SERVER_H
#define DEMO_HOST_TCP_SERVER_H

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "cJSON.h"
#include "net_abstraction.h"
#include "params.h"
#include "host_registry.h"
#include "host_data_sink.h"

/* wire v2：固定一对一 TCP 服务。在线 + pending 总数 ≤ PROTO_HOST_MAX_CONN，
 * 第二连接回复 host_ack busy reason=single_device_only 后关闭。
 * 控制面 JSON + 数据面 SERIAL_BYTES 统一走发送队列，处理 partial write。 */
class HostTcpServer {
public:
    HostTcpServer(net_ctx_t *net, HostRegistry &reg, const demo_params_t &params,
                  IHostDataSink *sink = nullptr);
    ~HostTcpServer();
    /* UI/主线程调用；sink_mu_ 保证 SetDataSink(nullptr) 返回后不再有回调
     * 正在使用旧 sink，窗口析构时释放 SerialDataModel 是安全的。 */
    void SetDataSink(IHostDataSink *sink)
    {
        std::lock_guard<std::mutex> lock(sink_mu_);
        sink_ = sink;
    }
    int Start(); /* 幂等：已有 listener 时返回 DEMO_OK */
    void Stop(); /* 幂等：关闭 pending/online 连接与 listener；可重复调用 */
    bool Started() const { return listen_ != nullptr; }
    void Poll(uint64_t now_ms);
    void BroadcastCloseAll();
    /* UI 线程调用：入队一条联调消息，由 Poll 在 host 线程冲刷发送。
     * 返回 DEMO_OK=已入队；DEMO_ERR=id/text 为空或 text 超 APP_DATA_TEXT_MAX */
    int QueueAppData(const std::string &device_id, const std::string &text);
    /* UI 线程调用：入队一帧已编码的串口消息（SERIAL_BYTES），由 Poll 冲刷发送。
     * 返回 DEMO_OK=已入队；DEMO_ERR=id 为空或 frame 为空/超长 */
    int QueueSerialFrame(const std::string &device_id,
                         const std::vector<uint8_t> &frame);

private:
    struct PendingConn {
        void *sock = nullptr;
        net_addr_t peer{};
        uint64_t connect_ms = 0;
        std::vector<uint8_t> rx;
        int malformed = 0;
    };

    struct SendCtx {
        std::deque<std::vector<uint8_t>> queue;

        std::vector<uint8_t> inflight;
        size_t inflight_off = 0;
        uint64_t tx_sequence = 0;
    };

    struct PendingSerialFrame {
        std::string device_id;
        std::vector<uint8_t> frame;
    };

    void HandlePending(PendingConn &pc, uint64_t now_ms);
    void HandleOnline(DeviceEntry &e, uint64_t now_ms);
    void FlushPendingTx();
    void FlushPendingSerialTx();
    void FlushTx();
    int SendFrame(void *sock, cJSON *obj);
    int SendSerialBytes(void *sock, const uint8_t *bytes, size_t length);
    std::string NewSessionId();
    void RejectBusy(void *conn);
    int ActiveConnCount() const; /* 在线(conn!=null) + pending */

    net_ctx_t *net_;
    HostRegistry &reg_;
    const demo_params_t &params_;
    IHostDataSink *sink_ = nullptr;
    std::mutex sink_mu_;
    void *listen_ = nullptr;
    std::vector<PendingConn> pending_;
    std::map<void *, std::vector<uint8_t>> conn_rx_; /* 已注册连接收包缓冲（按句柄隔离） */
    std::map<void *, SendCtx> send_ctx_;             /* 发送队列（按句柄隔离） */
    std::mutex tx_mu_;
    std::vector<std::pair<std::string, std::string>> pending_tx_; /* (device_id, text) */
    std::vector<PendingSerialFrame> pending_serial_frames_; /* (device_id, frame) */
    uint32_t app_data_seq_ = 0;
};

#endif
