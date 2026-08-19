#include "host_provision_client.h"
#include "cJSON.h"
#include "frame.h"
#include "protocol.h"
#include "sim_backend.h"
#include "log.h"
#include "pc_event.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

constexpr const char *kDeviceApPassword = "modutech_leventure";
constexpr const char *kProvisionPin = "5935";

/* 设备 wifi_result 帧 reason 文案 → wifi_reason_t（对应 device_provision.c 文案）；
 * 未知文案返回 -1。 */
int wifi_reason_from_text(const char *text)
{
    if (text == nullptr)
        return -1;
    if (strcmp(text, "信号太弱") == 0)
        return WIFI_REASON_NO_AP_FOUND;
    if (strcmp(text, "密码错误") == 0)
        return WIFI_REASON_AUTH_FAIL;
    if (strcmp(text, "超时") == 0)
        return WIFI_REASON_HANDSHAKE_TIMEOUT;
    if (strcmp(text, "目标为5G网络") == 0)
        return WIFI_REASON_5G_BAND;
    return -1;
}

} // namespace

int host_wifi_result_reason_from_frame(const cJSON *frame)
{
    if (frame == nullptr)
        return -1;
    const cJSON *why = cJSON_GetObjectItemCaseSensitive(frame, "reason");
    if (why == nullptr)
        return -1;
    if (cJSON_IsNumber(why))
        return why->valueint;
    if (cJSON_IsString(why))
        return wifi_reason_from_text(why->valuestring);
    return -1;
}

void host_emit_wifi_result_fail_event(const char *device_id, const cJSON *frame)
{
    char data[64];
    snprintf(data, sizeof(data), "{\"status\":\"fail\",\"reason\":%d}",
             host_wifi_result_reason_from_frame(frame));
    pc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = "wifi_result";
    ev.result = "fail";
    ev.code = DEMO_ERR;
    ev.device_id = device_id;
    ev.data_json = data;
    pc_events_emit(&ev);
}

HostProvisionClient::HostProvisionClient(net_ctx_t *net, const demo_params_t &params, bool real_wifi)
    : net_(net), params_(params), real_wifi_(real_wifi),
      target_ssid_(params.target_ssid), target_password_(params.target_password)
{
}

int HostProvisionClient::ScanWifiNetworks(std::vector<std::string> *ssids)
{
    ssids->clear();
    net_ap_info_t aps[64];
    int count = 64;
    if (net_wifi_scan(net_, aps, &count) != DEMO_OK)
        return DEMO_ERR;
    for (int index = 0; index < count; ++index) {
        if (aps[index].ssid[0] == '\0' || !aps[index].band_2g ||
            strncmp(aps[index].ssid, "Modu_", 5) == 0)
            continue;
        if (std::find(ssids->begin(), ssids->end(), aps[index].ssid) == ssids->end())
            ssids->emplace_back(aps[index].ssid);
    }
    return DEMO_OK;
}

void HostProvisionClient::SetTargetNetwork(const std::string &ssid,
                                           const std::string &password)
{
    std::lock_guard<std::mutex> lock(target_mutex_);
    target_ssid_ = ssid;
    target_password_ = password;
    LOG_I("HOST", "目标 WiFi 已更新：SSID=%s", target_ssid_.c_str());
}

int HostProvisionClient::ScanAps(std::vector<std::string> *modu_ssids)
{
    modu_ssids->clear();
    net_ap_info_t aps[32];
    int count = 32;
    if (net_wifi_scan(net_, aps, &count) != DEMO_OK)
        return DEMO_ERR;
    for (int i = 0; i < count; i++) {
        if (strncmp(aps[i].ssid, "Modu_", 5) == 0 && aps[i].band_2g)
            modu_ssids->push_back(aps[i].ssid);
    }
    LOG_I("HOST", "扫描完成：发现 %d 个热点，其中 %zu 个 Modu 设备热点", count,
          modu_ssids->size());
    return DEMO_OK;
}

/* 阻塞读一帧（带超时） */
static cJSON *read_frame(net_ctx_t *net, void *sock, int timeout_ms)
{
    static uint8_t acc[2048];
    static int acc_len = 0;
    acc_len = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t buf[512];
        int n = net_sock_recv(net, sock, buf, (int)sizeof(buf));
        if (n > 0) {
            memcpy(acc + acc_len, buf, (size_t)n);
            acc_len += n;
            int off = 0, len = 0, consumed = 0;
            int rc = frame_parse(acc, acc_len, &off, &len, &consumed);
            if (rc == 1) {
                acc[off + len] = '\0';
                cJSON *j = cJSON_Parse((const char *)(acc + off));
                memmove(acc, acc + consumed, (size_t)(acc_len - consumed));
                acc_len -= consumed;
                return j;
            }
            if (rc == DEMO_ERR) {
                memmove(acc, acc + consumed, (size_t)(acc_len - consumed));
                acc_len -= consumed;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nullptr;
}

/* 发送结果契约：
 * - frame_wrap 失败：原错误码原样返回
 * - 底层负错误码：原样返回
 * - 返回非负但小于帧长（部分写入）：DEMO_ERR
 * - 完整写入整帧：DEMO_OK
 * 本版本不实现循环重试。 */
static int send_frame(net_ctx_t *net, void *sock, const char *json)
{
    uint8_t frame[2048];
    int n = frame_wrap((const uint8_t *)json, (int)strlen(json), frame, (int)sizeof(frame));
    if (n < 0)
        return n;
    int rc = net_sock_send(net, sock, frame, n);
    if (rc < 0)
        return rc;
    return rc == n ? DEMO_OK : DEMO_ERR;
}

int HostProvisionClient::ProvisionDevice(const char *ap_ssid)
{
    std::string target_ssid;
    std::string target_password;
    {
        std::lock_guard<std::mutex> lock(target_mutex_);
        target_ssid = target_ssid_;
        target_password = target_password_;
    }
    if (target_ssid.empty() || target_password.size() < 8) {
        LOG_E("HOST", "配网：目标 WiFi SSID 为空或密码少于 8 位");
        return -1;
    }
    sim_ap_record_t ap{};
    if (!real_wifi_) {
        if (sim_backend_ap_find(&params_, ap_ssid, &ap) != DEMO_OK) {
            LOG_E("HOST", "配网：未找到模拟设备热点 %s", ap_ssid);
            return -1;
        }
    } else {
        snprintf(ap.ssid, sizeof(ap.ssid), "%s", ap_ssid);
        snprintf(ap.password, sizeof(ap.password), "%s", kDeviceApPassword);
        snprintf(ap.pin, sizeof(ap.pin), "%s", kProvisionPin);
    }

    wifi_reason_t reason = WIFI_REASON_OK;
    if (net_wifi_sta_connect(net_, ap.ssid, ap.password, &reason) != DEMO_OK) {
        if (reason == WIFI_REASON_HANDSHAKE_TIMEOUT)
            LOG_E("HOST", "配网：设备热点已关联，但设备 AP 未分配可用 IPv4；"
                  "请检查设备端 DHCP 服务和 AP 网关配置");
        else
            LOG_E("HOST", "配网：连接设备热点失败，原因码=%d", (int)reason);
        return -1;
    }
    LOG_I("HOST", "配网：已连接设备热点 %s", ap_ssid);

    net_addr_t addr;
    if (real_wifi_) {
        /* 以 DHCP 实际分配的网关作为设备 AP 地址（设备热点子网由 DHCP 决定，
         * 不可硬编码 PROTO_AP_IP，否则 169.254/异网段时 TCP 必然失败） */
        uint32_t gateway_ip = 0;
        int gateway_result = DEMO_ERR_AGAIN;
        for (int attempt = 0; attempt < 20; ++attempt) {
            gateway_result = net_wifi_get_gateway(net_, &gateway_ip);
            if (gateway_result == DEMO_OK && gateway_ip != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (gateway_result != DEMO_OK || gateway_ip == 0) {
            LOG_E("HOST", "配网：目标 WLAN 在 10 秒内未获得 DHCP 网关；"
                  "请检查设备 DHCP option 3 和 dnsmasq 状态");
            net_wifi_sta_disconnect(net_);
            return -1;
        }
        char gateway[16] = {};
        inet_ntop(AF_INET, &gateway_ip, gateway, sizeof(gateway));
        LOG_I("HOST", "配网：设备热点网关=%s（来自 DHCP）", gateway);
        addr.ip = gateway_ip;
        addr.port = htons(PROTO_TCP_PORT);
    } else {
        addr.ip = htonl(INADDR_LOOPBACK);
        addr.port = htons(ap.real_port);
    }
    void *conn = nullptr;
    int connect_result = DEMO_ERR;
    char target_ip[16] = {};
    inet_ntop(AF_INET, &addr.ip, target_ip, sizeof(target_ip));
    for (int attempt = 1; attempt <= 5; ++attempt) {
        LOG_I("HOST", "配网：TCP 连接设备 %s:%u，第 %d/5 次", target_ip,
              (unsigned)PROTO_TCP_PORT, attempt);
        connect_result = net_tcp_connect(net_, &addr, &conn, 3000);
        if (connect_result == DEMO_OK)
            break;
        if (attempt < 5)
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    if (connect_result != DEMO_OK) {
        LOG_E("HOST", "配网：连接设备热点的 TCP 服务失败，目标=%s:%u，返回码=%d",
              target_ip, (unsigned)PROTO_TCP_PORT, connect_result);
        net_wifi_sta_disconnect(net_);
        return -1;
    }

    int result = -1;
    /* 4. auth（PIN 模拟人工从设备标签输入） */
    {
        cJSON *auth = cJSON_CreateObject();
        cJSON_AddStringToObject(auth, "cmd", CMD_AUTH);
        cJSON_AddStringToObject(auth, "pin", ap.pin);
        char *s = cJSON_PrintUnformatted(auth);
        int auth_rc = send_frame(net_, conn, s);
        free(s);
        cJSON_Delete(auth);
        if (auth_rc != DEMO_OK) {
            LOG_E("HOST", "配网：auth 写入配网连接失败，返回码=%d，本次配网结束", auth_rc);
        } else {
            LOG_I("HOST", "配网：已发送 auth，PIN=%s（来自设备标签）", ap.pin);

            cJSON *ar = read_frame(net_, conn, params_.provision_auth_timeout_ms * 2);
            if (!ar) {
                LOG_E("HOST", "配网：等待 auth_result 超时");
                {
                    pc_event_t ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.event = "auth_result";
                    ev.result = "fail";
                    ev.code = DEMO_ERR_TIMEOUT;
                    ev.device_id = ap.device_id;
                    ev.data_json = "{\"status\":\"fail\"}";
                    pc_events_emit(&ev);
                }
            } else {
                const cJSON *st = cJSON_GetObjectItemCaseSensitive(ar, "status");
                if (cJSON_IsString(st) && strcmp(st->valuestring, "ok") == 0) {
                    {
                        pc_event_t ev;
                        memset(&ev, 0, sizeof(ev));
                        ev.event = "auth_result";
                        ev.result = "ok";
                        ev.code = DEMO_OK;
                        ev.device_id = ap.device_id;
                        ev.data_json = "{\"status\":\"ok\"}";
                        pc_events_emit(&ev);
                    }
                    /* 5. wifi_config */
                    cJSON *cfg = cJSON_CreateObject();
                    cJSON_AddStringToObject(cfg, "cmd", CMD_WIFI_CONFIG);
                    cJSON_AddStringToObject(cfg, "ssid", target_ssid.c_str());
                    cJSON_AddStringToObject(cfg, "password", target_password.c_str());
                    char *cs = cJSON_PrintUnformatted(cfg);
                    int cfg_rc = send_frame(net_, conn, cs);
                    free(cs);
                    cJSON_Delete(cfg);
                    if (cfg_rc != DEMO_OK) {
                        LOG_E("HOST", "配网：wifi_config 写入配网连接失败，返回码=%d，本次配网结束",
                              cfg_rc);
                    } else {
                        LOG_I("HOST", "配网：已向设备发送 wifi_config，要求设备连接 SSID=%s",
                              target_ssid.c_str());

                        cJSON *wr = read_frame(net_, conn, params_.provision_wifi_cfg_timeout_ms * 2);
                        if (!wr) {
                            LOG_E("HOST", "配网：等待 wifi_result 超时");
                            {
                                pc_event_t ev;
                                memset(&ev, 0, sizeof(ev));
                                ev.event = "wifi_result";
                                ev.result = "fail";
                                ev.code = DEMO_ERR_TIMEOUT;
                                ev.device_id = ap.device_id;
                                ev.data_json = "{\"status\":\"fail\",\"reason\":-5}";
                                pc_events_emit(&ev);
                            }
                        } else {
                            const cJSON *wst = cJSON_GetObjectItemCaseSensitive(wr, "status");
                            if (cJSON_IsString(wst) && strcmp(wst->valuestring, "ok") == 0) {
                                const cJSON *dip = cJSON_GetObjectItemCaseSensitive(wr, "device_ip");
                                LOG_I("HOST", "设备已接受目标 WiFi 配置：设备热点=%s，目标SSID=%s，设备IP=%s，等待设备回连",
                                      ap_ssid, target_ssid.c_str(),
                                      cJSON_IsString(dip) ? dip->valuestring : "?");
                                {
                                    pc_event_t ev;
                                    memset(&ev, 0, sizeof(ev));
                                    ev.event = "wifi_result";
                                    ev.result = "ok";
                                    ev.code = DEMO_OK;
                                    ev.device_id = ap.device_id;
                                    ev.data_json = "{\"status\":\"ok\",\"reason\":0}";
                                    pc_events_emit(&ev);
                                }
                                /* 6. close_ap（最佳努力，仅发送一次，不重试） */
                                cJSON *close = cJSON_CreateObject();
                                cJSON_AddStringToObject(close, "cmd", CMD_CLOSE_AP);
                                char *bs = cJSON_PrintUnformatted(close);
                                int close_rc = send_frame(net_, conn, bs);
                                free(bs);
                                cJSON_Delete(close);
                                if (close_rc == DEMO_OK)
                                    LOG_I("HOST", "配网：close_ap 已完整写入配网连接");
                                else
                                    LOG_E("HOST", "配网：close_ap 写入失败，返回码=%d，设备将自动完成交接",
                                          close_rc);
                                {
                                    char data[64];
                                    snprintf(data, sizeof(data), "{\"send_result\":%d}", close_rc);
                                    pc_event_t ev;
                                    memset(&ev, 0, sizeof(ev));
                                    ev.event = "close_ap_sent";
                                    ev.result = close_rc == DEMO_OK ? "ok" : "fail";
                                    ev.code = close_rc;
                                    ev.device_id = ap.device_id;
                                    ev.data_json = data;
                                    pc_events_emit(&ev);
                                }
                                /* 设备端具备自动交接，close_ap 失败不改变 WiFi 配置结果 */
                                result = 0;
                            } else {
                                const cJSON *why = cJSON_GetObjectItemCaseSensitive(wr, "reason");
                                LOG_E("HOST", "设备拒绝或无法连接目标 WiFi %s：%s",
                                      target_ssid.c_str(),
                                      cJSON_IsString(why) ? why->valuestring : "未知原因");
                                host_emit_wifi_result_fail_event(ap.device_id, wr);
                            }
                            cJSON_Delete(wr);
                        }
                    }
                } else {
                    LOG_E("HOST", "配网：auth 被设备拒绝");
                }
                cJSON_Delete(ar);
            }
        }
    }

    /* 结束配网连接：关闭配网 TCP → 断开设备热点 → 切回目标 WiFi */
    net_sock_close(net_, conn);
    LOG_I("HOST", "正在断开设备热点");
    net_wifi_sta_disconnect(net_);
    if (real_wifi_) {
        LOG_I("HOST", "正在切回目标 WiFi %s", target_ssid.c_str());
        wifi_reason_t restore_reason = WIFI_REASON_OK;
        if (net_wifi_sta_connect(net_, target_ssid.c_str(), target_password.c_str(),
                                 &restore_reason) != DEMO_OK)
            LOG_W("HOST", "未能自动切回目标 WiFi %s，原因码=%d，请手动连接",
                  target_ssid.c_str(), (int)restore_reason);
        else
            LOG_I("HOST", "已切回目标 WiFi %s", target_ssid.c_str());
    }
    LOG_I("HOST", "配网结果：%s %s", ap_ssid, result == 0 ? "成功" : "失败");
    return result;
}
