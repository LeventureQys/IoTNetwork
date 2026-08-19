#include "sim_world.h"
#include "protocol.h"
#include <cstdio>
#include <cstdlib>

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    if (!src) { if (cap) dst[0] = 0; return; }
    snprintf(dst, cap, "%s", src);
}

static uint32_t ip_of(const char *s)
{
    /* 点分十进制 → 网络字节序（与 inet_addr 一致） */
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0;
    return (uint32_t)((d << 24) | (c << 16) | (b << 8) | a);
}

SimWorld &SimWorld::Instance()
{
    static SimWorld world;
    return world;
}

void SimWorld::TargetNetworkSet(const char *ssid, const char *pass, bool band_2g)
{
    std::lock_guard<std::mutex> lk(mu_);
    copy_bounded(target_ssid_, sizeof(target_ssid_), ssid);
    copy_bounded(target_password_, sizeof(target_password_), pass);
    target_band_2g_ = band_2g;
    target_up_ = true;
    target_auth_fail_ = false;
}

void SimWorld::TargetSetUp(bool up) { std::lock_guard<std::mutex> lk(mu_); target_up_ = up; }
bool SimWorld::TargetUp() const { std::lock_guard<std::mutex> lk(mu_); return target_up_; }
void SimWorld::TargetSetAuthFail(bool on) { std::lock_guard<std::mutex> lk(mu_); target_auth_fail_ = on; }
bool SimWorld::TargetAuthFail() const { std::lock_guard<std::mutex> lk(mu_); return target_auth_fail_; }
const char *SimWorld::TargetSsid() const { std::lock_guard<std::mutex> lk(mu_); return target_ssid_; }
const char *SimWorld::TargetPassword() const { std::lock_guard<std::mutex> lk(mu_); return target_password_; }
bool SimWorld::TargetBand2g() const { std::lock_guard<std::mutex> lk(mu_); return target_band_2g_; }

void SimWorld::ApRegister(const char *owner_tag, const char *ssid, const char *pass,
                          const char *pin, uint16_t real_port)
{
    std::lock_guard<std::mutex> lk(mu_);
    /* 同 owner 先注销 */
    for (int i = 0; i < ap_count_; i++) {
        if (strcmp(aps_[i].owner_tag, owner_tag) == 0) {
            memset(&aps_[i], 0, sizeof(SimAp));
            copy_bounded(aps_[i].ssid, sizeof(aps_[i].ssid), ssid);
            copy_bounded(aps_[i].password, sizeof(aps_[i].password), pass);
            copy_bounded(aps_[i].pin, sizeof(aps_[i].pin), pin);
            copy_bounded(aps_[i].owner_tag, sizeof(aps_[i].owner_tag), owner_tag);
            aps_[i].real_port = real_port;
            aps_[i].active = true;
            return;
        }
    }
    if (ap_count_ >= (int)(sizeof(aps_) / sizeof(aps_[0])))
        return;
    SimAp &ap = aps_[ap_count_++];
    memset(&ap, 0, sizeof(ap));
    copy_bounded(ap.ssid, sizeof(ap.ssid), ssid);
    copy_bounded(ap.password, sizeof(ap.password), pass);
    copy_bounded(ap.pin, sizeof(ap.pin), pin);
    copy_bounded(ap.owner_tag, sizeof(ap.owner_tag), owner_tag);
    ap.real_port = real_port;
    ap.active = true;
}

void SimWorld::ApUnregister(const char *owner_tag)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < ap_count_; i++) {
        if (strcmp(aps_[i].owner_tag, owner_tag) == 0) {
            memset(&aps_[i], 0, sizeof(SimAp));
            aps_[i] = aps_[ap_count_ - 1];
            memset(&aps_[ap_count_ - 1], 0, sizeof(SimAp));
            ap_count_--;
            return;
        }
    }
}

bool SimWorld::ApFind(const char *ssid, SimAp *out) const
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < ap_count_; i++) {
        if (aps_[i].active && strcmp(aps_[i].ssid, ssid) == 0) {
            if (out) *out = aps_[i];
            return true;
        }
    }
    return false;
}

size_t SimWorld::ApCount() const { std::lock_guard<std::mutex> lk(mu_); return (size_t)ap_count_; }

bool SimWorld::ApIterate(int *pos, SimAp *out) const
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = *pos; i < ap_count_; i++) {
        if (aps_[i].active) {
            if (out) *out = aps_[i];
            *pos = i + 1;
            return true;
        }
    }
    return false;
}

int SimWorld::StaConnect(const char *ssid, const char *pass) const
{
    std::lock_guard<std::mutex> lk(mu_);
    /* 1) 虚拟 AP（host 连接设备热点场景）：密码匹配 → 成功 */
    for (int i = 0; i < ap_count_; i++) {
        if (aps_[i].active && strcmp(aps_[i].ssid, ssid) == 0) {
            if (strcmp(aps_[i].password, pass) == 0)
                return 0;
            return 202; /* WIFI_REASON_AUTH_FAIL */
        }
    }
    /* 2) 目标网络（设备 STA 连接场景） */
    if (strcmp(ssid, target_ssid_) != 0)
        return 201; /* WIFI_REASON_NO_AP_FOUND */
    if (!target_up_)
        return 201;
    if (target_auth_fail_ || strcmp(pass, target_password_) != 0)
        return 202; /* WIFI_REASON_AUTH_FAIL */
    if (!target_band_2g_)
        return 500; /* WIFI_REASON_5G_BAND */
    return 0;
}

void SimWorld::RssiSet(const char *tag, int rssi)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < rssi_count_; i++) {
        if (strcmp(rssi_[i].tag, tag) == 0) { rssi_[i].rssi = rssi; return; }
    }
    if (rssi_count_ < (int)(sizeof(rssi_) / sizeof(rssi_[0]))) {
        copy_bounded(rssi_[rssi_count_].tag, sizeof(rssi_[rssi_count_].tag), tag);
        rssi_[rssi_count_].rssi = rssi;
        rssi_count_++;
    }
}

int SimWorld::RssiGet(const char *tag) const
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < rssi_count_; i++)
        if (strcmp(rssi_[i].tag, tag) == 0)
            return rssi_[i].rssi;
    return -58; /* 默认 RSSI */
}

void SimWorld::MdnsRegister(const SimMdnsSvc &svc)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < mdns_count_; i++) {
        if (strcmp(mdns_[i].type, svc.type) == 0) {
            mdns_[i] = svc;
            return;
        }
    }
    if (mdns_count_ < (int)(sizeof(mdns_) / sizeof(mdns_[0])))
        mdns_[mdns_count_++] = svc;
}

void SimWorld::MdnsUnregister(const char *type)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < mdns_count_; i++) {
        if (strcmp(mdns_[i].type, type) == 0) {
            mdns_[i] = mdns_[mdns_count_ - 1];
            memset(&mdns_[mdns_count_ - 1], 0, sizeof(SimMdnsSvc));
            mdns_count_--;
            return;
        }
    }
}

bool SimWorld::MdnsResolve(const char *type, SimMdnsSvc *out) const
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < mdns_count_; i++) {
        if (mdns_[i].active && strcmp(mdns_[i].type, type) == 0) {
            if (out) *out = mdns_[i];
            return true;
        }
    }
    return false;
}

void SimWorld::McastSetBlocked(const char *tag, bool blocked)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < blocked_count_; i++) {
        if (strcmp(blocked_[i].tag, tag) == 0) { blocked_[i].blocked = blocked; return; }
    }
    if (blocked_count_ < (int)(sizeof(blocked_) / sizeof(blocked_[0]))) {
        copy_bounded(blocked_[blocked_count_].tag, sizeof(blocked_[blocked_count_].tag), tag);
        blocked_[blocked_count_].blocked = blocked;
        blocked_count_++;
    }
}

bool SimWorld::McastIsBlocked(const char *tag) const
{
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < blocked_count_; i++)
        if (strcmp(blocked_[i].tag, tag) == 0)
            return blocked_[i].blocked;
    return false;
}

uint32_t SimWorld::HostVirtualIp()       { return ip_of(PROTO_PC_AP_IP); }
uint32_t SimWorld::DeviceApVirtualIp()   { return ip_of("192.168.1.1"); }
uint32_t SimWorld::DeviceStaVirtualIp(int dev_index)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "192.168.1.%d", 100 + dev_index);
    return ip_of(buf);
}

uint16_t SimWorld::DeviceApRealPort(int base, int dev_index)
{
    return (uint16_t)(base + dev_index);
}
