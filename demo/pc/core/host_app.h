#ifndef DEMO_HOST_APP_H
#define DEMO_HOST_APP_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include "net_abstraction.h"
#include "params.h"
#include "host_registry.h"
#include "host_tcp_server.h"
#include "host_announcer.h"
#include "host_mdns.h"
#include "host_provision_client.h"

class HostApp {
public:
    HostApp(const demo_params_t &params, net_ctx_t *net, bool continuous_provision = false,
            bool real_wifi = false);
    ~HostApp();
    int Start();
    void Run();               /* 阻塞主循环，直到 stop */
    void RequestStop();       /* 优雅退出：SendBye → 关闭连接 */
    void ForceCrash();        /* 剧本 host_crash：不广播 bye 直接关闭 */
    void ProvisionAllDevices();
    int ScanWifiNetworks(std::vector<std::string> *ssids);
    void SetTargetNetwork(const std::string &ssid, const std::string &password);
    void SetBusyOverride(int limit);
    /* UI 线程调用：向指定设备入队一条联调消息（app_data），由主循环冲刷发送 */
    int SendAppDataToDevice(const std::string &id, const std::string &text);
    size_t OnlineCount() const;
    uint64_t announce_seq() const { return announcer_.last_seq(); }
    HostRegistry &registry_mut() { return registry_; }
    const HostRegistry &registry() const { return registry_; }
    bool Stopped() const { return stop_; }
    bool Started() const { return started_; }
    bool Provisioning() const { return provisioning_; }
    int ProvisionTotal() const { return provision_total_; }
    int ProvisionDone() const { return provision_done_; }

private:
    const demo_params_t &params_;
    net_ctx_t *net_;
    HostRegistry registry_;
    HostTcpServer tcp_server_;
    HostAnnouncer announcer_;
    HostMdns mdns_;
    HostProvisionClient provision_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> provisioning_{false};
    std::atomic<int> provision_total_{0};
    std::atomic<int> provision_done_{0};
    bool continuous_provision_ = false;
    std::thread provision_thread_;
    std::mutex provision_mutex_;
    std::unordered_set<std::string> provisioned_ssids_;
};

#endif
