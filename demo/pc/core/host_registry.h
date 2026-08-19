#ifndef DEMO_HOST_REGISTRY_H
#define DEMO_HOST_REGISTRY_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "net_abstraction.h"

struct DeviceEntry {
    std::string id;
    std::string fw_version;
    std::string session_id;
    std::string state; /* "online" / "offline" */
    int proto_ver = 1;
    uint64_t last_rx_ms = 0;
    uint64_t first_seen_ms = 0;
    uint64_t last_seen_ms = 0;
    uint32_t last_ping_seq = 0;
    uint32_t lost_ping_count = 0;
    int reconnect_count = 0;
    int last_rssi = 0;
    void *conn = nullptr; /* 抽象层 socket 句柄 */
};

class HostRegistry {
public:
    DeviceEntry *Find(const std::string &id);
    DeviceEntry *Add(const std::string &id, void *conn); /* 重复 id 替换条目（旧 conn 由调用方关闭） */
    void Remove(const std::string &id);
    void RemoveAll();
    size_t Size() const;
    void OnRx(const std::string &id, uint64_t now_ms);
    void OnPing(const std::string &id, uint32_t seq);
    void MarkOffline(const std::string &id);
    std::vector<std::string> FindDead(uint64_t now_ms, int dead_ms) const;
    std::vector<DeviceEntry *> AllOnline();
    std::vector<DeviceEntry> Snapshot() const; /* 锁内完整拷贝（UI 无竞争读取） */

private:
    std::map<std::string, DeviceEntry> entries_;
    mutable std::mutex mu_; /* main 线程查询与 host 线程更新并发访问保护 */
};

#endif
