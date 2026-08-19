#include "win_hotspot.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Networking::Connectivity;
using namespace winrt::Windows::Networking::NetworkOperators;

namespace {

struct RealImpl {
    NetworkOperatorTetheringManager manager{nullptr};
    bool started = false;
    std::string ssid;
};

std::string hresult_message(hresult_error const &e)
{
    return "HRESULT 0x" + std::to_string((unsigned long)(int32_t)e.code());
}

/* 找到可承载热点的 Wi-Fi 连接配置；无 Wi-Fi 时回退到任意带适配器的配置。 */
ConnectionProfile find_wifi_profile()
{
    auto profiles = NetworkInformation::GetConnectionProfiles();
    for (uint32_t i = 0; i < profiles.Size(); ++i) {
        ConnectionProfile profile = profiles.GetAt(i);
        if (profile.IsWlanConnectionProfile())
            return profile;
    }
    for (uint32_t i = 0; i < profiles.Size(); ++i) {
        ConnectionProfile profile = profiles.GetAt(i);
        if (profile.NetworkAdapter() != nullptr)
            return profile;
    }
    return nullptr;
}

/* 定位 Windows 移动热点承载适配器（Microsoft Wi-Fi Direct Virtual Adapter 或
 * 持有 192.168.137.x 的适配器），仅读取 IPv4/前缀，绝不修改其他网卡。 */
struct HotspotAdapterInfo {
    bool found = false;
    NET_IFINDEX ifindex = 0;
    std::string ipv4;
    int prefix = 0;
};

HotspotAdapterInfo query_hotspot_adapter()
{
    HotspotAdapterInfo info;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        return info;
    std::vector<unsigned char> storage(size);
    auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR)
        return info;
    for (auto *adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;
        const bool is_virtual =
            adapter->Description != nullptr &&
            wcsstr(adapter->Description, L"Wi-Fi Direct Virtual Adapter") != nullptr;
        for (auto *address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (!address->Address.lpSockaddr ||
                address->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            const auto *sin = reinterpret_cast<const sockaddr_in *>(
                address->Address.lpSockaddr);
            char buffer[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &sin->sin_addr, buffer, sizeof(buffer)))
                continue;
            if (is_virtual || strncmp(buffer, "192.168.137.", 12) == 0) {
                info.found = true;
                info.ifindex = adapter->IfIndex;
                info.ipv4 = buffer;
                info.prefix = (int)address->OnLinkPrefixLength;
                return info;
            }
        }
    }
    return info;
}

bool valid_ipv4_text(const char *ipv4)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = '\0';
    if (!ipv4 || sscanf(ipv4, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
        return false;
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

/* ---- 真实 WinRT ops ---- */

int real_create(void **out, std::string *error)
{
    (void)error;
    if (!out)
        return DEMO_ERR_INVAL;
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (const hresult_error &) {
        /* 线程已初始化 COM（可能 STA）；继续使用现有 apartment。 */
    }
    try {
        RealImpl *impl = new RealImpl();
        *out = impl;
        return DEMO_OK;
    } catch (const std::bad_alloc &) {
        return DEMO_ERR_NOMEM;
    }
}

void real_destroy(void *ctx)
{
    if (!ctx)
        return;
    RealImpl *impl = static_cast<RealImpl *>(ctx);
    if (impl->started) {
        try {
            impl->manager.StopTetheringAsync().get();
        } catch (...) {
            /* 析构路径不抛出，资源交由系统回收。 */
        }
    }
    delete impl;
}

int real_start(void *ctx, const char *ssid, const char *password, std::string *error)
{
    RealImpl *impl = static_cast<RealImpl *>(ctx);
    if (!impl)
        return DEMO_ERR_INVAL;
    if (!ssid || !ssid[0] || !password || !password[0]) {
        if (error) *error = "热点 SSID 或密码为空";
        return DEMO_ERR_INVAL;
    }
    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = strlen(password);
    if (ssid_len > 32 || pass_len < 8 || pass_len > 63) {
        if (error) *error = "热点 SSID 或密码长度非法";
        return DEMO_ERR_INVAL;
    }
    try {
        ConnectionProfile profile = find_wifi_profile();
        if (!profile) {
            if (error) *error = "未找到可承载热点的 WiFi 网卡";
            return DEMO_ERR;
        }
        TetheringCapability capability =
            NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile);
        if (capability != TetheringCapability::Enabled) {
            if (error) *error = "系统移动热点能力不可用（组策略/硬件/SKU 限制）";
            return DEMO_ERR;
        }
        NetworkOperatorTetheringManager manager =
            NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);

        NetworkOperatorTetheringAccessPointConfiguration config;
        config.Ssid(to_hstring(ssid));
        config.Passphrase(to_hstring(password));
        manager.ConfigureAccessPointAsync(config).get();

        NetworkOperatorTetheringOperationResult result = manager.StartTetheringAsync().get();
        if (result.Status() != TetheringOperationStatus::Success) {
            if (error) *error = "启动移动热点失败（系统返回非成功状态）";
            return DEMO_ERR;
        }

        bool on = false;
        for (int i = 0; i < 40; ++i) {
            TetheringOperationalState state = manager.TetheringOperationalState();
            if (state == TetheringOperationalState::On) {
                on = true;
                break;
            }
            Sleep(500);
        }
        if (!on) {
            manager.StopTetheringAsync().get();
            if (error) *error = "等待移动热点就绪超时";
            return DEMO_ERR_TIMEOUT;
        }
        impl->manager = manager;
        impl->started = true;
        impl->ssid = ssid;
        LOG_I("HOTSPOT", "Windows 移动热点已启动，SSID=%s", ssid);
        return DEMO_OK;
    } catch (const hresult_error &e) {
        if (error) {
            if (static_cast<int32_t>(e.code()) == static_cast<int32_t>(0x80070005) /* E_ACCESSDENIED */)
                *error = "移动热点权限不足";
            else
                *error = "移动热点操作失败：" + hresult_message(e);
        }
        LOG_E("HOTSPOT", "移动热点启动失败，SSID=%s，%s", ssid, hresult_message(e).c_str());
        return DEMO_ERR;
    } catch (...) {
        if (error) *error = "移动热点操作发生未知错误";
        LOG_E("HOTSPOT", "移动热点启动发生未知错误，SSID=%s", ssid);
        return DEMO_ERR;
    }
}

int real_query(void *ctx, net_ap_status_t *status, std::string *error)
{
    RealImpl *impl = static_cast<RealImpl *>(ctx);
    if (!impl || !status)
        return DEMO_ERR_INVAL;
    memset(status, 0, sizeof(*status));
    if (!impl->started) {
        if (error) *error = "热点未运行";
        return DEMO_ERR;
    }
    try {
        TetheringOperationalState state = impl->manager.TetheringOperationalState();
        status->started = state == TetheringOperationalState::On ? 1 : 0;
        if (!status->started) {
            if (error) *error = "热点未处于运行状态";
            return DEMO_ERR;
        }
        NetworkOperatorTetheringAccessPointConfiguration config =
            impl->manager.GetCurrentAccessPointConfiguration();
        const std::string ssid = to_string(config.Ssid());
        snprintf(status->ssid, sizeof(status->ssid), "%s", ssid.c_str());
        HotspotAdapterInfo info = query_hotspot_adapter();
        if (!info.found) {
            if (error) *error = "未找到热点承载适配器的 IPv4";
            return DEMO_ERR;
        }
        snprintf(status->ipv4, sizeof(status->ipv4), "%s", info.ipv4.c_str());
        status->prefix_length = info.prefix;
        return DEMO_OK;
    } catch (const hresult_error &e) {
        if (error) *error = "查询热点状态失败：" + hresult_message(e);
        return DEMO_ERR;
    } catch (...) {
        if (error) *error = "查询热点状态发生未知错误";
        return DEMO_ERR;
    }
}

int real_configure_ipv4(void *ctx, const char *ipv4, int prefix_length, std::string *error)
{
    RealImpl *impl = static_cast<RealImpl *>(ctx);
    if (!impl)
        return DEMO_ERR_INVAL;
    if (!valid_ipv4_text(ipv4)) {
        if (error) *error = "IPv4 地址非法";
        return DEMO_ERR_INVAL;
    }
    if (prefix_length <= 0 || prefix_length > 32) {
        if (error) *error = "前缀长度非法";
        return DEMO_ERR_INVAL;
    }
    if (!impl->started) {
        if (error) *error = "热点未运行";
        return DEMO_ERR;
    }
    HotspotAdapterInfo info = query_hotspot_adapter();
    if (!info.found) {
        if (error) *error = "未找到热点承载适配器，无法限定 IP 配置范围";
        return DEMO_ERR;
    }
    if (info.ipv4 == ipv4 && info.prefix == prefix_length) {
        LOG_I("HOTSPOT", "热点承载适配器 IPv4 已为目标值 %s/%d", ipv4, prefix_length);
        return DEMO_OK;
    }
    /* 仅对目标热点承载适配器设置静态地址；失败即 fail-closed。 */
    MIB_UNICASTIPADDRESS_ROW row;
    InitializeUnicastIpAddressEntry(&row);
    row.InterfaceIndex = info.ifindex;
    row.Address.Ipv4.sin_family = AF_INET;
    row.Address.Ipv4.sin_addr.s_addr = inet_addr(ipv4);
    row.OnLinkPrefixLength = (UINT8)prefix_length;
    row.ValidLifetime = 0xffffffff;
    row.PreferredLifetime = 0xffffffff;
    DWORD rc = SetUnicastIpAddressEntry(&row);
    if (rc != NO_ERROR) {
        if (error) *error = "设置热点承载适配器 IPv4 失败（错误码 " + std::to_string(rc) + "）";
        LOG_E("HOTSPOT", "设置热点适配器 IPv4 失败，错误码=%lu", (unsigned long)rc);
        return DEMO_ERR;
    }
    LOG_I("HOTSPOT", "热点承载适配器 IPv4 已配置为 %s/%d", ipv4, prefix_length);
    return DEMO_OK;
}

int real_stop(void *ctx, std::string *error)
{
    RealImpl *impl = static_cast<RealImpl *>(ctx);
    if (!impl)
        return DEMO_ERR_INVAL;
    if (!impl->started) {
        return DEMO_OK; /* 幂等 */
    }
    try {
        impl->manager.StopTetheringAsync().get();
    } catch (const hresult_error &e) {
        if (error) *error = "停止移动热点失败：" + hresult_message(e);
        return DEMO_ERR;
    } catch (...) {
        if (error) *error = "停止移动热点发生未知错误";
        return DEMO_ERR;
    }
    impl->started = false;
    impl->ssid.clear();
    LOG_I("HOTSPOT", "Windows 移动热点已停止");
    return DEMO_OK;
}

const WinHotspotOps g_real_ops = {
    real_create,
    real_destroy,
    real_start,
    real_query,
    real_configure_ipv4,
    real_stop,
};

} // namespace

const WinHotspotOps *win_hotspot_real_ops()
{
    return &g_real_ops;
}

WinHotspot::WinHotspot() : ops_(win_hotspot_real_ops()), owns_ctx_(true)
{
}

WinHotspot::WinHotspot(const WinHotspotOps *ops, void *ctx)
    : ops_(ops), ctx_(ctx), owns_ctx_(false)
{
}

WinHotspot::~WinHotspot()
{
    if (owns_ctx_ && ops_ && ctx_) {
        ops_->destroy(ctx_);
        ctx_ = nullptr;
    }
}

int WinHotspot::Start(const char *ssid, const char *password, std::string *error)
{
    if (!ops_)
        return DEMO_ERR;
    if (owns_ctx_ && !ctx_) {
        int rc = ops_->create(&ctx_, error);
        if (rc != DEMO_OK)
            return rc;
    }
    return ops_->start(ctx_, ssid, password, error);
}

int WinHotspot::Query(net_ap_status_t *status, std::string *error)
{
    if (!ops_)
        return DEMO_ERR;
    if (owns_ctx_ && !ctx_) {
        int rc = ops_->create(&ctx_, error);
        if (rc != DEMO_OK)
            return rc;
    }
    return ops_->query(ctx_, status, error);
}

int WinHotspot::ConfigureIpv4(const char *ipv4, int prefix_length, std::string *error)
{
    if (!ops_)
        return DEMO_ERR;
    if (owns_ctx_ && !ctx_) {
        int rc = ops_->create(&ctx_, error);
        if (rc != DEMO_OK)
            return rc;
    }
    return ops_->configure_ipv4(ctx_, ipv4, prefix_length, error);
}

int WinHotspot::Stop(std::string *error)
{
    if (!ops_)
        return DEMO_ERR;
    if (owns_ctx_ && !ctx_) {
        return DEMO_OK; /* 从未启动：幂等成功 */
    }
    return ops_->stop(ctx_, error);
}
