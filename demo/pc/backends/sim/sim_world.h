#ifndef DEMO_SIM_WORLD_H
#define DEMO_SIM_WORLD_H

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

/* 进程级共享虚拟网络世界：模拟 WiFi 介质、mDNS 注册表、RSSI、组播屏蔽。
 * 全部方法线程安全（内部互斥锁）。多实例（host ×1 + device ×N）共享。 */

struct SimAp {
    char ssid[33];
    char password[64];
    char pin[8];
    uint16_t real_port;
    char owner_tag[16];
    bool active;
};

struct SimMdnsSvc {
    char instance[64];
    char type[64];
    uint32_t ip;       /* 网络字节序 */
    uint16_t port;     /* 网络字节序 */
    char txt[128];
    bool active;
};

class SimWorld {
public:
    static SimWorld &Instance();

    /* 目标网络（启动时注册一次） */
    void TargetNetworkSet(const char *ssid, const char *pass, bool band_2g);
    void TargetSetUp(bool up);
    bool TargetUp() const;
    void TargetSetAuthFail(bool on);
    bool TargetAuthFail() const;
    const char *TargetSsid() const;
    const char *TargetPassword() const;
    bool TargetBand2g() const;

    /* 虚拟 AP */
    void ApRegister(const char *owner_tag, const char *ssid, const char *pass,
                    const char *pin, uint16_t real_port);
    void ApUnregister(const char *owner_tag);
    bool ApFind(const char *ssid, SimAp *out) const;
    size_t ApCount() const;
    bool ApIterate(int *pos, SimAp *out) const;   /* pos 从 0 起，找到返回 true 并递增 pos */

    /* STA 连接判定：返回 0=ok 否则 wifi_reason_t */
    int StaConnect(const char *ssid, const char *pass) const;

    /* RSSI（按 tag） */
    void RssiSet(const char *tag, int rssi);
    int RssiGet(const char *tag) const;

    /* mDNS 注册表 */
    void MdnsRegister(const SimMdnsSvc &svc);
    void MdnsUnregister(const char *type);
    bool MdnsResolve(const char *type, SimMdnsSvc *out) const;

    /* 组播屏蔽（按 tag，屏蔽后该实例收不到组播） */
    void McastSetBlocked(const char *tag, bool blocked);
    bool McastIsBlocked(const char *tag) const;

    /* 虚拟 IP */
    static uint32_t HostVirtualIp();
    static uint32_t DeviceApVirtualIp();
    static uint32_t DeviceStaVirtualIp(int dev_index);
    static uint16_t DeviceApRealPort(int base, int dev_index);

private:
    SimWorld() = default;
    mutable std::mutex mu_;

    char target_ssid_[33] = {0};
    char target_password_[64] = {0};
    bool target_band_2g_ = true;
    bool target_up_ = true;
    bool target_auth_fail_ = false;

    SimAp aps_[16];
    int ap_count_ = 0;

    struct RssiEntry { char tag[16]; int rssi; };
    RssiEntry rssi_[17];
    int rssi_count_ = 0;

    SimMdnsSvc mdns_[4];
    int mdns_count_ = 0;

    struct BlockEntry { char tag[16]; bool blocked; };
    BlockEntry blocked_[17];
    int blocked_count_ = 0;
};

#endif
