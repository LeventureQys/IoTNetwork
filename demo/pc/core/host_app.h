#ifndef DEMO_HOST_APP_H
#define DEMO_HOST_APP_H

#include <atomic>
#include <cstdint>
#include <string>
#include "net_abstraction.h"
#include "params.h"
#include "host_registry.h"
#include "host_tcp_server.h"

/* beta v1.1：一对一 PC 热点直连生命周期（设计文档 8.1）。
 * Start() 顺序：热点启动 → status → 必要时配置固定 IP → 再次 status → TCP start。
 * 任一步失败按逆序回滚。不再包含 announcer/mDNS/provision client。 */
class HostApp {
public:
    HostApp(const demo_params_t &params, net_ctx_t *net);
    ~HostApp();
    int Start();
    void Run();               /* 阻塞主循环，直到 stop */
    void RequestStop();       /* 优雅退出：关闭 TCP → 停止热点 */
    void ForceCrash();        /* 剧本 host_crash：直接关闭，不广播 */
    /* UI 线程调用：向指定设备入队一条联调消息（app_data），由主循环冲刷发送 */
    int SendAppDataToDevice(const std::string &id, const std::string &text);
    size_t OnlineCount() const;
    const demo_params_t &params() const { return params_; }
    HostRegistry &registry_mut() { return registry_; }
    const HostRegistry &registry() const { return registry_; }
    bool Stopped() const { return stop_; }
    bool Started() const { return started_; }

private:
    const demo_params_t &params_;
    net_ctx_t *net_;
    HostRegistry registry_;
    HostTcpServer tcp_server_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};
};

#endif
