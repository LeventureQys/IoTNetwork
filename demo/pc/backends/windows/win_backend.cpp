#include "win_backend.h"
#include "win_hotspot.h"
#include "pc_socket_backend.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wlanapi.h>
#include <objbase.h>
#include <iphlpapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct WinBackend {
    void *socket_user;
    WinHotspot *hotspot = nullptr;
    HANDLE wlan = nullptr;
    DWORD wlan_version = 0;
    GUID connected_interface{};
    bool connected = false;
};

const net_backend_t *socket_backend()
{
    return pc_socket_backend_table();
}

std::string xml_escape(const char *text)
{
    std::string escaped;
    for (const char *cursor = text; cursor && *cursor; ++cursor) {
        switch (*cursor) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '\"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default: escaped += *cursor; break;
        }
    }
    return escaped;
}

bool ssid_equals(const DOT11_SSID &ssid, const char *value)
{
    size_t length = strlen(value);
    return length == ssid.uSSIDLength &&
           memcmp(ssid.ucSSID, value, length) == 0;
}

std::string ipv4_for_interface(const GUID &interface_guid)
{
    NET_LUID luid{};
    NET_IFINDEX interface_index = 0;
    if (ConvertInterfaceGuidToLuid(&interface_guid, &luid) != NO_ERROR ||
        ConvertInterfaceLuidToIndex(&luid, &interface_index) != NO_ERROR)
        return {};

    ULONG size = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                         GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &size);
    std::vector<unsigned char> storage(size);
    IP_ADAPTER_ADDRESSES *adapters =
        reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size) != NO_ERROR)
        return {};
    for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->IfIndex != interface_index)
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *address = adapter->FirstUnicastAddress;
             address; address = address->Next) {
            const sockaddr_in *ipv4 =
                reinterpret_cast<const sockaddr_in *>(address->Address.lpSockaddr);
            char buffer[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer)))
                return buffer;
        }
    }
    return {};
}

bool is_apipa_address(const std::string &address)
{
    return address.rfind("169.254.", 0) == 0;
}

int open_wlan(WinBackend *backend)
{
    if (backend->wlan)
        return DEMO_OK;
    DWORD result = WlanOpenHandle(2, nullptr, &backend->wlan_version, &backend->wlan);
    if (result != ERROR_SUCCESS) {
        LOG_E("WLAN", "WlanOpenHandle 失败，错误码=%lu", result);
        return DEMO_ERR;
    }
    return DEMO_OK;
}

int wlan_scan(void *user, net_ap_info_t *aps, int *count)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (open_wlan(backend) != DEMO_OK)
        return DEMO_ERR;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    DWORD result = WlanEnumInterfaces(backend->wlan, nullptr, &interfaces);
    if (result != ERROR_SUCCESS || !interfaces)
        return DEMO_ERR;

    for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index)
        WlanScan(backend->wlan, &interfaces->InterfaceInfo[index].InterfaceGuid,
                 nullptr, nullptr, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    int capacity = *count;
    int found = 0;
    for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
        PWLAN_BSS_LIST list = nullptr;
        result = WlanGetNetworkBssList(backend->wlan,
                                       &interfaces->InterfaceInfo[index].InterfaceGuid,
                                       nullptr, dot11_BSS_type_any, FALSE, nullptr, &list);
        if (result != ERROR_SUCCESS || !list)
            continue;
        for (DWORD item = 0; item < list->dwNumberOfItems; ++item) {
            const WLAN_BSS_ENTRY &entry = list->wlanBssEntries[item];
            if (entry.dot11Ssid.uSSIDLength == 0 || entry.dot11Ssid.uSSIDLength > 32)
                continue;
            char ssid[33] = {};
            memcpy(ssid, entry.dot11Ssid.ucSSID, entry.dot11Ssid.uSSIDLength);
            bool duplicate = false;
            for (int existing = 0; existing < found && existing < capacity; ++existing) {
                if (strcmp(aps[existing].ssid, ssid) == 0) {
                    duplicate = true;
                    if (entry.lRssi > aps[existing].rssi)
                        aps[existing].rssi = entry.lRssi;
                    break;
                }
            }
            if (duplicate)
                continue;
            if (found < capacity) {
                net_ap_info_t &ap = aps[found];
                memset(&ap, 0, sizeof(ap));
                snprintf(ap.ssid, sizeof(ap.ssid), "%s", ssid);
                ap.rssi = entry.lRssi;
                ap.band_2g = entry.ulChCenterFrequency >= 2400000 &&
                             entry.ulChCenterFrequency < 2500000;
                snprintf(ap.bssid, sizeof(ap.bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                         entry.dot11Bssid[0], entry.dot11Bssid[1], entry.dot11Bssid[2],
                         entry.dot11Bssid[3], entry.dot11Bssid[4], entry.dot11Bssid[5]);
            }
            found++;
        }
        WlanFreeMemory(list);
    }
    WlanFreeMemory(interfaces);
    *count = found < capacity ? found : capacity;
    return DEMO_OK;
}

int wlan_connect(void *user, const char *ssid, const char *password, wifi_reason_t *reason)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (reason) *reason = WIFI_REASON_NO_AP_FOUND;
    if (!ssid || !password || strlen(ssid) > 32 || strlen(password) < 8 || strlen(password) > 63)
        return DEMO_ERR_INVAL;
    if (open_wlan(backend) != DEMO_OK)
        return DEMO_ERR;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(backend->wlan, nullptr, &interfaces) != ERROR_SUCCESS || !interfaces)
        return DEMO_ERR;

    std::string profile_name = std::string("ModuProvision-") + ssid;
    std::string xml =
        "<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        "<name>" + xml_escape(profile_name.c_str()) + "</name><SSIDConfig><SSID><name>" +
        xml_escape(ssid) + "</name></SSID><nonBroadcast>false</nonBroadcast></SSIDConfig>"
        "<connectionType>ESS</connectionType><connectionMode>manual</connectionMode>"
        "<MSM><security><authEncryption><authentication>WPA2PSK</authentication>"
        "<encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
        "<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>" +
        xml_escape(password) + "</keyMaterial></sharedKey></security></MSM></WLANProfile>";
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, xml.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wide_xml((size_t)wide_length);
    MultiByteToWideChar(CP_UTF8, 0, xml.c_str(), -1, wide_xml.data(), wide_length);

    int profile_length = MultiByteToWideChar(CP_UTF8, 0, profile_name.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wide_profile((size_t)profile_length);
    MultiByteToWideChar(CP_UTF8, 0, profile_name.c_str(), -1, wide_profile.data(), profile_length);

    int result_code = DEMO_ERR;
    for (DWORD index = 0; index < interfaces->dwNumberOfItems; ++index) {
        DWORD reason_code = 0;
        DWORD result = WlanSetProfile(backend->wlan,
                                      &interfaces->InterfaceInfo[index].InterfaceGuid,
                                      WLAN_PROFILE_USER, wide_xml.data(), nullptr, TRUE,
                                      nullptr, &reason_code);
        if (result != ERROR_SUCCESS)
            continue;
        WLAN_CONNECTION_PARAMETERS connection{};
        connection.wlanConnectionMode = wlan_connection_mode_profile;
        connection.strProfile = wide_profile.data();
        connection.dot11BssType = dot11_BSS_type_infrastructure;
        result = WlanConnect(backend->wlan,
                             &interfaces->InterfaceInfo[index].InterfaceGuid,
                             &connection, nullptr);
        if (result != ERROR_SUCCESS)
            continue;
        for (int attempt = 0; attempt < 30; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
            DWORD size = 0;
            WLAN_OPCODE_VALUE_TYPE opcode;
            result = WlanQueryInterface(backend->wlan,
                                        &interfaces->InterfaceInfo[index].InterfaceGuid,
                                        wlan_intf_opcode_current_connection, nullptr,
                                        &size, reinterpret_cast<PVOID *>(&attributes), &opcode);
            if (result == ERROR_SUCCESS && attributes &&
                attributes->isState == wlan_interface_state_connected &&
                ssid_equals(attributes->wlanAssociationAttributes.dot11Ssid, ssid)) {
                backend->connected_interface = interfaces->InterfaceInfo[index].InterfaceGuid;
                backend->connected = true;
                if (reason) *reason = WIFI_REASON_OK;
                result_code = DEMO_OK;
                LOG_I("WLAN", "已关联 SSID=%s，等待 DHCP/路由就绪", ssid);
                WlanFreeMemory(attributes);
                break;
            }
            if (attributes)
                WlanFreeMemory(attributes);
        }
        if (result_code == DEMO_OK)
            break;
    }
    WlanFreeMemory(interfaces);
    if (result_code == DEMO_OK) {
        std::string last_address;
        for (int attempt = 0; attempt < 20; ++attempt) {
            last_address = ipv4_for_interface(backend->connected_interface);
            if (!last_address.empty() && !is_apipa_address(last_address))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (last_address.empty()) {
            LOG_W("WLAN", "SSID=%s 已关联，但 10 秒内未获得 IPv4 地址", ssid);
            result_code = DEMO_ERR_TIMEOUT;
        } else if (is_apipa_address(last_address)) {
            LOG_E("WLAN", "SSID=%s 已关联，但仅获得 Windows 自分配地址 %s；"
                  "设备 AP 的 DHCP 服务未响应，无法获取设备网关",
                  ssid, last_address.c_str());
            result_code = DEMO_ERR_TIMEOUT;
        } else {
            LOG_I("WLAN", "SSID=%s 网络就绪，本机 IPv4=%s", ssid, last_address.c_str());
        }
        if (result_code != DEMO_OK) {
            WlanDisconnect(backend->wlan, &backend->connected_interface, nullptr);
            backend->connected = false;
            if (reason) *reason = WIFI_REASON_HANDSHAKE_TIMEOUT;
        }
    }
    if (result_code != DEMO_OK && reason && *reason == WIFI_REASON_NO_AP_FOUND)
        *reason = WIFI_REASON_AUTH_FAIL;
    return result_code;
}

int wlan_disconnect(void *user)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (backend->wlan && backend->connected)
        WlanDisconnect(backend->wlan, &backend->connected_interface, nullptr);
    backend->connected = false;
    return DEMO_OK;
}

int backend_init(void *user, const char *)
{
    return open_wlan(static_cast<WinBackend *>(user));
}

void backend_deinit(void *user)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (backend->wlan) {
        WlanCloseHandle(backend->wlan, nullptr);
        backend->wlan = nullptr;
    }
}

int hotspot_ap_start(void *user, const char *ssid, const char *pass, const char *pin)
{
    (void)pin; /* 设计文档 7.1：PIN 参数必须忽略 */
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (!backend->hotspot)
        return DEMO_ERR;
    std::string error;
    int rc = backend->hotspot->Start(ssid, pass, &error);
    if (rc != DEMO_OK)
        LOG_E("WLAN", "移动热点启动失败，SSID=%s：%s", ssid ? ssid : "",
              error.c_str());
    return rc;
}

int hotspot_ap_stop(void *user)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (!backend->hotspot)
        return DEMO_OK; /* 无热点组件：幂等停止 */
    std::string error;
    int rc = backend->hotspot->Stop(&error);
    if (rc != DEMO_OK)
        LOG_W("WLAN", "移动热点停止失败：%s", error.c_str());
    return rc;
}

int hotspot_ap_status(void *user, net_ap_status_t *status)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (!backend->hotspot)
        return DEMO_ERR;
    std::string error;
    int rc = backend->hotspot->Query(status, &error);
    if (rc != DEMO_OK)
        LOG_W("WLAN", "查询移动热点状态失败：%s", error.c_str());
    return rc;
}

int hotspot_ap_configure_ipv4(void *user, const char *ipv4, int prefix_length)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (!backend->hotspot)
        return DEMO_ERR;
    std::string error;
    int rc = backend->hotspot->ConfigureIpv4(ipv4, prefix_length, &error);
    if (rc != DEMO_OK)
        LOG_E("WLAN", "配置热点承载适配器 IPv4=%s/%d 失败：%s",
              ipv4 ? ipv4 : "", prefix_length, error.c_str());
    return rc;
}

int socket_wifi_rssi(void *, int *) { return DEMO_ERR; }
int socket_wifi_ip(void *, uint32_t *ip)
{
    char address[16] = {};
    if (win_backend_get_ipv4(address, sizeof(address)) != DEMO_OK)
        return DEMO_ERR;
    *ip = inet_addr(address);
    return DEMO_OK;
}

bool connected_interface_index(const WinBackend *backend, NET_IFINDEX *interface_index)
{
    NET_LUID luid{};
    return backend && backend->connected && interface_index &&
           ConvertInterfaceGuidToLuid(&backend->connected_interface, &luid) == NO_ERROR &&
           ConvertInterfaceLuidToIndex(&luid, interface_index) == NO_ERROR;
}

int gateway_for_connected_interface(WinBackend *backend, uint32_t *gateway_ip)
{
    if (!gateway_ip)
        return DEMO_ERR_INVAL;
    *gateway_ip = 0;
    NET_IFINDEX target_index = 0;
    if (!connected_interface_index(backend, &target_index))
        return DEMO_ERR;

    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG size = 0;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
    if (result != ERROR_BUFFER_OVERFLOW || size == 0)
        return DEMO_ERR;
    std::vector<unsigned char> storage(size);
    IP_ADAPTER_ADDRESSES *adapters =
        reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
    result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (result != NO_ERROR)
        return DEMO_ERR;

    for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->IfIndex != target_index || adapter->OperStatus != IfOperStatusUp)
            continue;
        for (IP_ADAPTER_GATEWAY_ADDRESS_LH *entry = adapter->FirstGatewayAddress;
             entry; entry = entry->Next) {
            if (!entry->Address.lpSockaddr ||
                entry->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            const sockaddr_in *gateway = reinterpret_cast<const sockaddr_in *>(
                entry->Address.lpSockaddr);
            uint32_t host_ip = ntohl(gateway->sin_addr.s_addr);
            if (host_ip == 0 || (host_ip >> 24) == 127 ||
                (host_ip & 0xFFFF0000u) == 0xA9FE0000u ||
                (host_ip & 0xF0000000u) == 0xE0000000u)
                continue;
            *gateway_ip = gateway->sin_addr.s_addr;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, gateway_ip, text, sizeof(text));
            LOG_I("WLAN", "已从目标 WLAN 接口 IfIndex=%lu 获取 DHCP 网关=%s",
                  (unsigned long)target_index, text);
            return DEMO_OK;
        }
        LOG_D("WLAN", "目标 WLAN 接口 IfIndex=%lu 尚无 IPv4 网关",
              (unsigned long)target_index);
        return DEMO_ERR_AGAIN;
    }
    return DEMO_ERR_AGAIN;
}
int wlan_get_current_ssid(void *user, char *ssid, int capacity)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (!ssid || capacity <= 0 || !backend->connected || !backend->wlan)
        return DEMO_ERR;
    PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
    DWORD size = 0;
    WLAN_OPCODE_VALUE_TYPE opcode;
    DWORD result = WlanQueryInterface(backend->wlan, &backend->connected_interface,
                                      wlan_intf_opcode_current_connection, nullptr,
                                      &size, reinterpret_cast<PVOID *>(&attributes), &opcode);
    if (result != ERROR_SUCCESS || !attributes)
        return DEMO_ERR;
    const DOT11_SSID &current = attributes->wlanAssociationAttributes.dot11Ssid;
    int length = static_cast<int>(current.uSSIDLength);
    if (length <= 0 || length >= capacity) {
        WlanFreeMemory(attributes);
        return DEMO_ERR;
    }
    memcpy(ssid, current.ucSSID, static_cast<size_t>(length));
    ssid[length] = '\0';
    WlanFreeMemory(attributes);
    return DEMO_OK;
}
int wlan_get_gateway(void *user, uint32_t *ip)
{
    return gateway_for_connected_interface(static_cast<WinBackend *>(user), ip);
}
int socket_tcp_listen(void *, uint16_t port, void **socket_out)
{
    SOCKET socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_value == INVALID_SOCKET) {
        LOG_E("TCP", "监听 socket 创建失败，端口=%u，WSA=%d", (unsigned)port,
              WSAGetLastError());
        return DEMO_ERR;
    }
    BOOL exclusive = TRUE;
    if (setsockopt(socket_value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
        LOG_W("TCP", "设置 SO_EXCLUSIVEADDRUSE 失败，端口=%u，WSA=%d", (unsigned)port,
              WSAGetLastError());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(socket_value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
        LOG_E("TCP", "监听 bind 失败：0.0.0.0:%u，WSA=%d", (unsigned)port,
              WSAGetLastError());
        closesocket(socket_value);
        return DEMO_ERR;
    }
    if (listen(socket_value, SOMAXCONN) == SOCKET_ERROR) {
        LOG_E("TCP", "监听 listen 失败：0.0.0.0:%u，WSA=%d", (unsigned)port,
              WSAGetLastError());
        closesocket(socket_value);
        return DEMO_ERR;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket_value, FIONBIO, &nonblocking) == SOCKET_ERROR)
        LOG_W("TCP", "监听 socket 设置非阻塞失败，端口=%u，WSA=%d", (unsigned)port,
              WSAGetLastError());
    LOG_I("TCP", "TCP 服务正在监听 0.0.0.0:%u", (unsigned)port);
    *socket_out = reinterpret_cast<void *>(socket_value);
    return DEMO_OK;
}
int socket_tcp_accept(void *u, void *l, void **c, net_addr_t *p) { WinBackend *b = static_cast<WinBackend *>(u); return socket_backend()->tcp_accept(b->socket_user, l, c, p); }
int socket_tcp_connect(void *, const net_addr_t *target, void **socket_out, int timeout_ms)
{
    char target_ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &target->ip, target_ip, sizeof(target_ip));
    LOG_I("TCP", "开始连接 %s:%u，超时=%dms", target_ip,
          (unsigned)ntohs(target->port), timeout_ms);
    SOCKET socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_value == INVALID_SOCKET) {
        LOG_E("TCP", "创建 socket 失败，WSA=%d", WSAGetLastError());
        return DEMO_ERR;
    }
    u_long nonblocking = 1;
    ioctlsocket(socket_value, FIONBIO, &nonblocking);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = target->ip;
    address.sin_port = target->port;
    int result = connect(socket_value, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    int connect_error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (result == SOCKET_ERROR && connect_error != WSAEWOULDBLOCK &&
        connect_error != WSAEINPROGRESS) {
        LOG_E("TCP", "connect %s:%u 立即失败，WSA=%d", target_ip,
              (unsigned)ntohs(target->port), connect_error);
        closesocket(socket_value);
        return DEMO_ERR;
    }
    if (result == SOCKET_ERROR) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket_value, &writable);
        timeval timeout{};
        int wait_ms = timeout_ms > 0 ? timeout_ms : 3000;
        timeout.tv_sec = wait_ms / 1000;
        timeout.tv_usec = (wait_ms % 1000) * 1000;
        if (select(0, nullptr, &writable, nullptr, &timeout) <= 0) {
            int select_error = WSAGetLastError();
            LOG_E("TCP", "等待连接 %s:%u 超时或失败，WSA=%d", target_ip,
                  (unsigned)ntohs(target->port), select_error);
            closesocket(socket_value);
            return DEMO_ERR_TIMEOUT;
        }
        int socket_error = 0;
        int error_size = sizeof(socket_error);
        getsockopt(socket_value, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char *>(&socket_error), &error_size);
        if (socket_error != 0) {
            LOG_E("TCP", "连接 %s:%u 失败，SO_ERROR/WSA=%d", target_ip,
                  (unsigned)ntohs(target->port), socket_error);
            closesocket(socket_value);
            return DEMO_ERR;
        }
    }
    LOG_I("TCP", "已连接 %s:%u", target_ip, (unsigned)ntohs(target->port));
    *socket_out = reinterpret_cast<void *>(socket_value);
    return DEMO_OK;
}
int socket_send(void *u, void *s, const uint8_t *d, int n) { WinBackend *b = static_cast<WinBackend *>(u); return socket_backend()->sock_send(b->socket_user, s, d, n); }
int socket_recv(void *u, void *s, uint8_t *d, int n) { WinBackend *b = static_cast<WinBackend *>(u); return socket_backend()->sock_recv(b->socket_user, s, d, n); }
void socket_close(void *u, void *s) { WinBackend *b = static_cast<WinBackend *>(u); socket_backend()->sock_close(b->socket_user, s); }
int socket_mcast_join(void *u, const char *g, uint16_t p, void **s) { WinBackend *b = static_cast<WinBackend *>(u); return socket_backend()->udp_mcast_join(b->socket_user, g, p, s); }
int socket_udp_send(void *u, const char *g, uint16_t p, const uint8_t *d, int n) { WinBackend *b = static_cast<WinBackend *>(u); return socket_backend()->udp_send(b->socket_user, g, p, d, n); }
int socket_udp_recv(void *u, void *s, uint8_t *d, int n, net_addr_t *f) { WinBackend *b = static_cast<WinBackend *>(u); return socket_backend()->udp_recv(b->socket_user, s, d, n, f); }
int unsupported_mdns_register(void *, const net_mdns_service_t *) { return DEMO_ERR; }
int unsupported_mdns_unregister(void *, const char *) { return DEMO_ERR; }
int unsupported_mdns_resolve(void *, const char *, net_mdns_service_t *, int) { return DEMO_ERR; }
int unsupported_nvs_get(void *, const char *, uint8_t *, int *) { return DEMO_ERR; }
int unsupported_nvs_set(void *, const char *, const uint8_t *, int) { return DEMO_ERR; }
int unsupported_nvs_erase(void *, const char *) { return DEMO_ERR; }
uint64_t socket_time(void *u) { WinBackend *b = static_cast<WinBackend *>(u); return socket_backend()->time_ms(b->socket_user); }
uint32_t socket_random(void *u) { WinBackend *b = static_cast<WinBackend *>(u); return socket_backend()->random(b->socket_user); }
int unsupported_inject(void *, const char *, const char *) { return DEMO_ERR; }

const net_backend_t backend_table = {
    backend_init, backend_deinit,
    wlan_scan, wlan_connect, wlan_disconnect,
    hotspot_ap_start, hotspot_ap_stop, hotspot_ap_status, hotspot_ap_configure_ipv4,
    socket_wifi_rssi, socket_wifi_ip,
    wlan_get_current_ssid, wlan_get_gateway,
    socket_tcp_listen, socket_tcp_accept, socket_tcp_connect,
    socket_send, socket_recv, socket_close,
    socket_mcast_join, socket_udp_send, socket_udp_recv,
    unsupported_mdns_register, unsupported_mdns_unregister, unsupported_mdns_resolve,
    unsupported_nvs_get, unsupported_nvs_set, unsupported_nvs_erase,
    socket_time, socket_random, unsupported_inject
};

} // namespace

extern "C" {

int win_backend_ipv4_valid(const char *ip)
{
    if (!ip)
        return DEMO_ERR_INVAL;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = '\0';
    if (sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
        return DEMO_ERR;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return DEMO_ERR;
    if (a == 0)                    /* unspecified 0.0.0.0/8 */
        return DEMO_ERR;
    if (a == 127)                  /* loopback 127.0.0.0/8 */
        return DEMO_ERR;
    if (a == 169 && b == 254)      /* APIPA 169.254.0.0/16 */
        return DEMO_ERR;
    return DEMO_OK;
}

void *win_backend_create(const demo_params_t *params)
{
    (void)params;
    WinBackend *backend = new WinBackend{};
    backend->socket_user = pc_socket_backend_create();
    if (!backend->socket_user) {
        delete backend;
        return nullptr;
    }
    backend->hotspot = new WinHotspot();
    return backend;
}

void win_backend_destroy(void *user)
{
    WinBackend *backend = static_cast<WinBackend *>(user);
    if (!backend)
        return;
    delete backend->hotspot;
    backend->hotspot = nullptr;
    pc_socket_backend_destroy(backend->socket_user);
    delete backend;
}

const net_backend_t *win_backend_table(void)
{
    return &backend_table;
}

int win_backend_get_ipv4(char *buffer, int capacity)
{
    if (!buffer || capacity < 16)
        return DEMO_ERR_INVAL;
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                         GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &size);
    std::vector<unsigned char> storage(size);
    IP_ADAPTER_ADDRESSES *adapters =
        reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size) != NO_ERROR)
        return DEMO_ERR;
    for (IP_ADAPTER_ADDRESSES *adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *address = adapter->FirstUnicastAddress;
             address; address = address->Next) {
            if (address->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            sockaddr_in *ipv4 = reinterpret_cast<sockaddr_in *>(address->Address.lpSockaddr);
            if (!inet_ntop(AF_INET, &ipv4->sin_addr, buffer, capacity))
                continue;
            if (win_backend_ipv4_valid(buffer) != DEMO_OK)
                continue;
            return DEMO_OK;
        }
    }
    return DEMO_ERR;
}

}
