#ifndef DEMO_HOST_PROVISION_CLIENT_H
#define DEMO_HOST_PROVISION_CLIENT_H

#include <string>
#include <mutex>
#include <vector>
#include "cJSON.h"
#include "net_abstraction.h"
#include "params.h"

class HostProvisionClient {
public:
    HostProvisionClient(net_ctx_t *net, const demo_params_t &params, bool real_wifi);
    int ScanAps(std::vector<std::string> *modu_ssids); /* 过滤 Modu_ 前缀且 2.4G */
    int ScanWifiNetworks(std::vector<std::string> *ssids);
    void SetTargetNetwork(const std::string &ssid, const std::string &password);
    int ProvisionDevice(const char *ap_ssid);          /* 0=ok -1=失败 */

private:
    net_ctx_t *net_;
    const demo_params_t &params_;
    bool real_wifi_;
    std::mutex target_mutex_;
    std::string target_ssid_;
    std::string target_password_;
};

/* 设计文档第 11 节：wifi_result 事件 data.reason:number 必须为设备帧真实原因。
 * 从设备 wifi_result 帧解析 reason 数值：
 * - 数字字段原样返回（201/202/205/500 等 wifi_reason_t）
 * - 字符串字段按设备端文案映射（"信号太弱"→201、"密码错误"→202、
 *   "超时"→205、"目标为5G网络"→500）
 * - 缺省/类型不符/未知文案：返回 -1（文档化缺省值） */
int host_wifi_result_reason_from_frame(const cJSON *frame);

/* 按设计文档第 11 节契约上报 wifi_result fail 事件：
 * data={"status":"fail","reason":<帧解析值>} */
void host_emit_wifi_result_fail_event(const char *device_id, const cJSON *frame);

#endif
