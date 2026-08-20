#include "win_hotspot.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ipifcons.h>

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
    NET_IFINDEX source_ifindex = 0;
    bool started = false;
    std::string ssid;
};

std::string hresult_message(hresult_error const &e)
{
    return "HRESULT 0x" + std::to_string((unsigned long)(int32_t)e.code());
}

/* TetheringOperationStatus 枚举值（SDK windows.networking.networkoperators.0.h）的中文/英文名。 */
const char *tethering_status_name(TetheringOperationStatus status)
{
    switch (status) {
    case TetheringOperationStatus::Success: return "Success";
    case TetheringOperationStatus::Unknown: return "Unknown";
    case TetheringOperationStatus::MobileBroadbandDeviceOff: return "MobileBroadbandDeviceOff";
    case TetheringOperationStatus::WiFiDeviceOff: return "WiFiDeviceOff";
    case TetheringOperationStatus::EntitlementCheckTimeout: return "EntitlementCheckTimeout";
    case TetheringOperationStatus::EntitlementCheckFailure: return "EntitlementCheckFailure";
    case TetheringOperationStatus::OperationInProgress: return "OperationInProgress";
    case TetheringOperationStatus::BluetoothDeviceOff: return "BluetoothDeviceOff";
    case TetheringOperationStatus::NetworkLimitedConnectivity: return "NetworkLimitedConnectivity";
    case TetheringOperationStatus::AlreadyOn: return "AlreadyOn";
    case TetheringOperationStatus::RadioRestriction: return "RadioRestriction";
    case TetheringOperationStatus::BandInterference: return "BandInterference";
    }
    return "UnknownStatus";
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

/* 定位 Windows 移动热点承载适配器。
 * 现代移动热点在启动后由 ICS 异步创建 Microsoft Wi-Fi Direct Virtual Adapter，
 * 其创建和 IPv4 分配均可能晚于 TetheringOperationalState=On；描述名还会随系统语言变化。
 * 识别优先级：
 *   1. 持有 192.168.137.x 的 Up 适配器（ICS 已收敛）
 *   2. 描述名包含 Wi-Fi Direct / Virtual Adapter / 虚拟适配器的 Up 适配器（本地化兜底）
 *   3. 非源网卡的 Up Wi-Fi 适配器（源网卡在 Start 时记录并排除）
 * 调用方在热点刚启动后应轮询本函数，等待承载适配器与 IPv4 出现。 */
struct HotspotAdapterInfo {
    bool found = false;
    NET_IFINDEX ifindex = 0;
    std::string ipv4;
    int prefix = 0;
};

bool has_hosted_adapter_description(const wchar_t *description)
{
    if (!description)
        return false;
    return wcsstr(description, L"Wi-Fi Direct") != nullptr ||
           wcsstr(description, L"Direct Virtual Adapter") != nullptr ||
           wcsstr(description, L"Virtual Adapter") != nullptr ||
           wcsstr(description, L"虚拟适配器") != nullptr;
}

bool interface_index_from_connection_profile(const ConnectionProfile &profile,
                                             NET_IFINDEX *out_index)
{
    if (!profile || !out_index)
        return false;
    const NetworkAdapter adapter = profile.NetworkAdapter();
    if (adapter == nullptr)
        return false;
    const winrt::guid adapter_id = adapter.NetworkAdapterId();
    GUID guid{};
    std::memcpy(&guid, &adapter_id, sizeof(guid));
    NET_LUID luid{};
    return ConvertInterfaceGuidToLuid(&guid, &luid) == NO_ERROR &&
           ConvertInterfaceLuidToIndex(&luid, out_index) == NO_ERROR;
}

HotspotAdapterInfo query_hotspot_adapter(NET_IFINDEX source_ifindex)
{
    HotspotAdapterInfo info;
    HotspotAdapterInfo described_ok;  /* 描述名匹配的承载适配器，非 APIPA */
    HotspotAdapterInfo described_any; /* 描述名匹配的承载适配器，任意 IPv4 */
    HotspotAdapterInfo wifi_ok;       /* 非源 Wi-Fi 适配器，非 APIPA */
    HotspotAdapterInfo wifi_any;      /* 非源 Wi-Fi 适配器，任意 IPv4 */
    bool have_described_ok = false;
    bool have_described_any = false;
    bool have_wifi_ok = false;
    bool have_wifi_any = false;
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
        /* 源网卡（对外上网网卡）绝不能被当作热点承载适配器改写；
         * 但若源 profile 本身就是 Wi-Fi Direct 虚拟适配器（例如热点已由系统开启），
         * 仍按描述名识别为承载适配器。 */
        const bool description_match =
            has_hosted_adapter_description(adapter->Description);
        if (source_ifindex != 0 && adapter->IfIndex == source_ifindex &&
            !description_match)
            continue;
        const bool wifi_interface = adapter->IfType == IF_TYPE_IEEE80211;
        const bool candidate = description_match ||
            (source_ifindex != 0 && wifi_interface);
        if (!candidate)
            continue;
        for (auto *address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (!address->Address.lpSockaddr ||
                address->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            const auto *sin = reinterpret_cast<const sockaddr_in *>(
                address->Address.lpSockaddr);
            char buffer[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &sin->sin_addr, buffer, sizeof(buffer)))
                continue;
            /* 第一优先：已持有 ICS 目标网段（192.168.137.x）的 Up 适配器 */
            if (strncmp(buffer, "192.168.137.", 12) == 0) {
                info.found = true;
                info.ifindex = adapter->IfIndex;
                info.ipv4 = buffer;
                info.prefix = (int)address->OnLinkPrefixLength;
                return info;
            }
            const bool apipa = strncmp(buffer, "169.254.", 8) == 0;
            HotspotAdapterInfo *slot = nullptr;
            bool *have_slot = nullptr;
            if (description_match) {
                slot = apipa ? &described_any : &described_ok;
                have_slot = apipa ? &have_described_any : &have_described_ok;
            } else {
                slot = apipa ? &wifi_any : &wifi_ok;
                have_slot = apipa ? &have_wifi_any : &have_wifi_ok;
            }
            if (slot && have_slot && !*have_slot) {
                slot->found = true;
                slot->ifindex = adapter->IfIndex;
                slot->ipv4 = buffer;
                slot->prefix = (int)address->OnLinkPrefixLength;
                *have_slot = true;
            }
        }
    }
    if (have_described_ok)
        return described_ok;
    if (have_described_any)
        return described_any;
    if (have_wifi_ok)
        return wifi_ok;
    return wifi_any;
}

/* 热点启动后承载适配器与 IPv4 异步出现：250ms * (attempts-1) 轮询窗口。 */
constexpr int kHotspotAdapterWaitAttempts = 40; /* 约 10 秒 */

HotspotAdapterInfo wait_for_hotspot_adapter(NET_IFINDEX source_ifindex, int attempts)
{
    HotspotAdapterInfo info;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        info = query_hotspot_adapter(source_ifindex);
        if (info.found && !info.ipv4.empty())
            return info;
        if (attempt == 0)
            LOG_I("HOTSPOT", "等待热点承载适配器 IPv4 就绪（最多 10 秒）");
        if (attempt + 1 < attempts)
            Sleep(250);
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

        impl->source_ifindex = 0;
        if (!interface_index_from_connection_profile(profile, &impl->source_ifindex))
            LOG_W("HOTSPOT", "无法解析热点源网卡索引，承载适配器将退化为描述名匹配");
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
        const TetheringOperationStatus start_status = result.Status();
        if (start_status != TetheringOperationStatus::Success &&
            start_status != TetheringOperationStatus::AlreadyOn) {
            if (error) {
                std::string reason = "启动移动热点失败：系统返回 " +
                                     std::string(tethering_status_name(start_status));
                if (start_status == TetheringOperationStatus::Unknown)
                    reason += "（热点服务可能卡死，请重启 Windows 移动热点服务或重启系统后重试）";
                *error = reason;
            }
            LOG_E("HOTSPOT", "移动热点启动失败，SSID=%s，状态=%s", ssid,
                  tethering_status_name(start_status));
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
        HotspotAdapterInfo info = wait_for_hotspot_adapter(
            impl->source_ifindex, kHotspotAdapterWaitAttempts);
        if (!info.found) {
            if (error) *error = "热点已开启，但 10 秒内未找到承载适配器的 IPv4";
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
    HotspotAdapterInfo info = wait_for_hotspot_adapter(
        impl->source_ifindex, kHotspotAdapterWaitAttempts);
    if (!info.found) {
        if (error) *error = "10 秒内未找到热点承载适配器，无法限定 IP 配置范围";
        return DEMO_ERR;
    }
    if (info.ipv4 == ipv4 && info.prefix == prefix_length) {
        LOG_I("HOTSPOT", "热点承载适配器 IPv4 已为目标值 %s/%d", ipv4, prefix_length);
        return DEMO_OK;
    }
    /* ICS 在热点启动后会自动分配承载网段（默认 192.168.137.1/24）。
     * 若当前是 APIPA（169.254.x，Windows 自分配地址），说明 ICS 尚未完成配置；
     * 先等待其收敛，避免与系统地址管理竞争（也避免刚启动即强制停止热点）。 */
    if (info.ipv4.rfind("169.254.", 0) == 0) {
        for (int attempt = 0; attempt < 24; ++attempt) { /* 24 * 500ms = 12 秒 */
            Sleep(500);
            HotspotAdapterInfo latest = query_hotspot_adapter(impl->source_ifindex);
            if (latest.found) {
                if (latest.ipv4 == ipv4 && latest.prefix == prefix_length) {
                    LOG_I("HOTSPOT", "ICS 已将热点承载适配器配置为目标值 %s/%d",
                          ipv4, prefix_length);
                    return DEMO_OK;
                }
                info = latest; /* 用最新快照继续后续强制配置 */
            }
        }
        LOG_W("HOTSPOT", "等待 ICS 配置 %s/%d 超时，尝试强制设置（需要管理员权限）",
              ipv4, prefix_length);
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
        std::string reason =
            "设置热点承载适配器 IPv4 失败（错误码 " + std::to_string(rc) + "）";
        if (rc == ERROR_ACCESS_DENIED)
            reason += "：缺少管理员权限，请以管理员身份运行";
        if (error) *error = reason;
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
        NetworkOperatorTetheringOperationResult result = impl->manager.StopTetheringAsync().get();
        if (result.Status() != TetheringOperationStatus::Success) {
            if (error) *error = "停止移动热点失败：系统返回 " +
                                std::string(tethering_status_name(result.Status()));
            LOG_E("HOTSPOT", "停止移动热点失败：%s",
                  tethering_status_name(result.Status()));
            return DEMO_ERR;
        }
        /* 确认状态真正变为 Off，避免残留热点（返回值可能为假成功） */
        bool off = false;
        for (int i = 0; i < 20; ++i) { /* 最多 5 秒 */
            if (impl->manager.TetheringOperationalState() == TetheringOperationalState::Off) {
                off = true;
                break;
            }
            Sleep(250);
        }
        if (!off) {
            if (error) *error = "停止移动热点超时：状态未在 5 秒内变为 Off";
            LOG_W("HOTSPOT", "停止移动热点后状态未在 5 秒内变为 Off");
            return DEMO_ERR_TIMEOUT;
        }
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
