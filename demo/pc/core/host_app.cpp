#include "host_app.h"
#include "log.h"
#include "pc_event.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

void emit_hotspot_error(const char *operation, const char *reason)
{
    char data[256];
    snprintf(data, sizeof(data), "{\"operation\":\"%s\",\"reason\":\"%s\"}",
             operation, reason);
    pc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = "hotspot_error";
    ev.result = "fail";
    ev.code = DEMO_ERR;
    ev.data_json = data;
    pc_events_emit(&ev);
}

} // namespace

HostApp::HostApp(const demo_params_t &params, net_ctx_t *net)
    : params_(params), net_(net), tcp_server_(net, registry_, params)
{
}

HostApp::~HostApp()
{
    ForceCrash();
}

int HostApp::Start()
{
    if (started_)
        return DEMO_OK;

    /* 1. 热点启动（PIN 传 NULL，按契约忽略） */
    int rc = net_wifi_ap_start(net_, params_.pc_ap_ssid, params_.pc_ap_password, nullptr);
    if (rc != DEMO_OK) {
        LOG_E("HOST", "热点启动失败，SSID=%s", params_.pc_ap_ssid);
        emit_hotspot_error("wifi_ap_start", "hotspot_start_failed");
        return rc;
    }

    /* 2. 查询热点实际状态 */
    net_ap_status_t status;
    rc = net_wifi_ap_status(net_, &status);
    if (rc != DEMO_OK) {
        LOG_E("HOST", "查询热点状态失败");
        emit_hotspot_error("wifi_ap_status", "hotspot_status_failed");
        net_wifi_ap_stop(net_);
        return rc;
    }

    /* 3. IP/前缀不匹配时配置固定 IP，并重新查询 */
    if (strcmp(status.ipv4, params_.pc_ap_ip) != 0 ||
        status.prefix_length != params_.pc_ap_prefix_length) {
        LOG_I("HOST", "热点 IP/前缀与目标不符（当前=%s/%d），尝试配置为 %s/%d",
              status.ipv4, status.prefix_length, params_.pc_ap_ip,
              params_.pc_ap_prefix_length);
        rc = net_wifi_ap_configure_ipv4(net_, params_.pc_ap_ip,
                                        params_.pc_ap_prefix_length);
        if (rc != DEMO_OK) {
            LOG_E("HOST", "配置热点固定 IP 失败");
            emit_hotspot_error("wifi_ap_configure_ipv4", "configure_ipv4_failed");
            net_wifi_ap_stop(net_);
            return rc;
        }
        rc = net_wifi_ap_status(net_, &status);
        if (rc != DEMO_OK) {
            emit_hotspot_error("wifi_ap_status", "hotspot_status_failed");
            net_wifi_ap_stop(net_);
            return rc;
        }
    }

    /* 4. 最终校验 SSID/IP/前缀，任一不匹配则回滚热点 */
    if (strcmp(status.ssid, params_.pc_ap_ssid) != 0 ||
        strcmp(status.ipv4, params_.pc_ap_ip) != 0 ||
        status.prefix_length != params_.pc_ap_prefix_length) {
        LOG_E("HOST", "热点 SSID/IP/前缀仍不匹配：ssid=%s ip=%s/%d",
              status.ssid, status.ipv4, status.prefix_length);
        emit_hotspot_error("wifi_ap_status", "hotspot_mismatch");
        net_wifi_ap_stop(net_);
        return DEMO_ERR;
    }

    /* 5. TCP 监听；失败回滚热点 */
    rc = tcp_server_.Start();
    if (rc != DEMO_OK) {
        LOG_E("HOST", "TCP 服务监听失败，端口=%d", params_.host_tcp_port);
        emit_hotspot_error("tcp_start", "tcp_start_failed");
        net_wifi_ap_stop(net_);
        return rc;
    }

    /* 6. 结构化事件：hotspot_ready 先于 tcp_listening（均不含密码） */
    {
        char data[160];
        snprintf(data, sizeof(data), "{\"ssid\":\"%s\",\"ip\":\"%s\",\"prefix_length\":%d}",
                 params_.pc_ap_ssid, params_.pc_ap_ip, params_.pc_ap_prefix_length);
        pc_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = "hotspot_ready";
        ev.result = "ok";
        ev.code = DEMO_OK;
        ev.data_json = data;
        pc_events_emit(&ev);
    }
    {
        char data[64];
        snprintf(data, sizeof(data), "{\"ip\":\"0.0.0.0\",\"port\":%d}",
                 params_.host_tcp_port);
        pc_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = "tcp_listening";
        ev.result = "ok";
        ev.code = DEMO_OK;
        ev.data_json = data;
        pc_events_emit(&ev);
    }

    started_ = true;
    LOG_I("HOST", "上位机已启动：热点=%s，IP=%s/%d，TCP端口=%d",
          params_.pc_ap_ssid, params_.pc_ap_ip, params_.pc_ap_prefix_length,
          params_.host_tcp_port);
    return DEMO_OK;
}

void HostApp::Run()
{
    while (!stop_) {
        uint64_t now = net_time_ms(net_);
        tcp_server_.Poll(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOG_I("HOST", "上位机运行循环已退出");
}

void HostApp::RequestStop()
{
    if (stop_)
        return;
    stop_ = true;
    LOG_I("HOST", "上位机正在优雅停止");
    tcp_server_.Stop();
    net_wifi_ap_stop(net_);
    started_ = false;
}

void HostApp::ForceCrash()
{
    if (stop_)
        return;
    stop_ = true;
    LOG_W("HOST", "上位机模拟崩溃（不广播 bye）");
    tcp_server_.Stop();
    net_wifi_ap_stop(net_);
    started_ = false;
}

int HostApp::SendAppDataToDevice(const std::string &id, const std::string &text)
{
    return tcp_server_.QueueAppData(id, text);
}

int HostApp::SendSerialFrameToDevice(const std::string &id,
                                     const std::vector<uint8_t> &frame)
{
    return tcp_server_.QueueSerialFrame(id, frame);
}

size_t HostApp::OnlineCount() const
{
    HostRegistry &r = const_cast<HostRegistry &>(registry_);
    return r.AllOnline().size();
}
