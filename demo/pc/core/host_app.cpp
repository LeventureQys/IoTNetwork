#include "host_app.h"
#include "log.h"

#include <thread>

HostApp::HostApp(const demo_params_t &params, net_ctx_t *net, bool continuous_provision,
                 bool real_wifi)
    : params_(params), net_(net),
      tcp_server_(net, registry_, params),
      announcer_(net, params),
      mdns_(net, params),
      provision_(net, params, real_wifi), continuous_provision_(continuous_provision)
{
}

HostApp::~HostApp()
{
    ForceCrash();
}

int HostApp::Start()
{
    int rc = tcp_server_.Start();
    if (rc != DEMO_OK)
        return rc;
    rc = announcer_.Start();
    if (rc != DEMO_OK) {
        tcp_server_.Stop();
        return rc;
    }
    if (mdns_.Register() != DEMO_OK)
        LOG_W("HOST", "mDNS 兼容注册失败；真实发现以 UDP 组播为主，继续运行");
    LOG_I("HOST", "上位机已启动，通告IP=%s，TCP端口=%d，组播=%s:%d",
          params_.host_virtual_ip, params_.host_tcp_port, params_.mcast_group,
          params_.mcast_port);
    started_ = true;
    if (continuous_provision_) {
        provision_thread_ = std::thread([this] {
            LOG_I("HOST", "后台持续扫描已启动，间隔 3 秒");
            while (!stop_) {
                ProvisionAllDevices();
                for (int elapsed = 0; elapsed < 30 && !stop_; ++elapsed)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    return DEMO_OK;
}

void HostApp::Run()
{
    while (!stop_) {
        uint64_t now = net_time_ms(net_);
        announcer_.Poll(now);
        tcp_server_.Poll(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOG_I("HOST", "上位机运行循环已退出");
}

void HostApp::RequestStop()
{
    if (stop_)
        return;
    LOG_I("HOST", "上位机正在优雅停止");
    announcer_.SendBye();
    announcer_.Stop();
    tcp_server_.Stop();
    mdns_.Unregister();
    stop_ = true;
    started_ = false;
    if (provision_thread_.joinable())
        provision_thread_.join();
}

void HostApp::ForceCrash()
{
    if (stop_)
        return;
    LOG_W("HOST", "上位机模拟崩溃（不发送 host_bye）");
    announcer_.Stop();
    tcp_server_.Stop();
    mdns_.Unregister();
    stop_ = true;
    started_ = false;
    if (provision_thread_.joinable())
        provision_thread_.join();
}

void HostApp::ProvisionAllDevices()
{
    std::unique_lock<std::mutex> lock(provision_mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    std::vector<std::string> aps;
    if (provision_.ScanAps(&aps) != DEMO_OK)
        return;
    provisioning_ = true;
    provision_total_ = (int)aps.size();
    provision_done_ = 0;
    for (const std::string &ssid : aps) {
        if (provisioned_ssids_.find(ssid) != provisioned_ssids_.end())
            continue;
        LOG_I("HOST", "--- 正在为设备 %s 配网 ---", ssid.c_str());
        if (provision_.ProvisionDevice(ssid.c_str()) == 0)
            provisioned_ssids_.insert(ssid);
        provision_done_++;
    }
    provisioning_ = false;
}

int HostApp::ScanWifiNetworks(std::vector<std::string> *ssids)
{
    std::unique_lock<std::mutex> lock(provision_mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return DEMO_ERR_AGAIN;
    return provision_.ScanWifiNetworks(ssids);
}

void HostApp::SetTargetNetwork(const std::string &ssid, const std::string &password)
{
    provision_.SetTargetNetwork(ssid, password);
}

void HostApp::SetBusyOverride(int limit)
{
    tcp_server_.SetBusyOverride(limit);
}

int HostApp::SendAppDataToDevice(const std::string &id, const std::string &text)
{
    return tcp_server_.QueueAppData(id, text);
}

size_t HostApp::OnlineCount() const
{
    HostRegistry &r = const_cast<HostRegistry &>(registry_);
    return r.AllOnline().size();
}
