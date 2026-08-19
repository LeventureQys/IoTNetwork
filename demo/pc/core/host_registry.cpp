#include "host_registry.h"
#include "log.h"
#include <algorithm>

DeviceEntry *HostRegistry::Find(const std::string &id)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

DeviceEntry *HostRegistry::Add(const std::string &id, void *conn)
{
    std::lock_guard<std::mutex> lk(mu_);
    DeviceEntry &e = entries_[id];
    bool existed = !e.id.empty();
    e.id = id;
    e.conn = conn;
    e.state = "online";
    if (!existed)
        e.first_seen_ms = e.last_seen_ms = 0; /* 由 OnRx 刷新 */
    return &e;
}

void HostRegistry::Remove(const std::string &id)
{
    std::lock_guard<std::mutex> lk(mu_);
    entries_.erase(id);
}

void HostRegistry::RemoveAll()
{
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
}

size_t HostRegistry::Size() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

void HostRegistry::OnRx(const std::string &id, uint64_t now_ms)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end())
        return;
    DeviceEntry &e = it->second;
    e.last_rx_ms = now_ms;
    e.last_seen_ms = now_ms;
    if (e.first_seen_ms == 0)
        e.first_seen_ms = now_ms;
}

void HostRegistry::OnPing(const std::string &id, uint32_t seq)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end())
        return;
    DeviceEntry &e = it->second;
    if (seq > e.last_ping_seq) {
        if (e.last_ping_seq > 0)
            e.lost_ping_count += (seq - e.last_ping_seq - 1);
        e.last_ping_seq = seq;
    }
}

void HostRegistry::MarkOffline(const std::string &id)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it != entries_.end())
        it->second.state = "offline";
}

std::vector<std::string> HostRegistry::FindDead(uint64_t now_ms, int dead_ms) const
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> dead;
    for (const auto &kv : entries_) {
        const DeviceEntry &e = kv.second;
        if (e.state == "online" && e.last_rx_ms > 0 &&
            now_ms - e.last_rx_ms > (uint64_t)dead_ms)
            dead.push_back(e.id);
    }
    return dead;
}

std::vector<DeviceEntry *> HostRegistry::AllOnline()
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<DeviceEntry *> out;
    for (auto &kv : entries_)
        if (kv.second.state == "online")
            out.push_back(&kv.second);
    return out;
}

std::vector<DeviceEntry> HostRegistry::Snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<DeviceEntry> out;
    for (const auto &kv : entries_)
        out.push_back(kv.second);
    return out;
}
