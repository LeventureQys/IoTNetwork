#ifndef DEMO_HOST_DATA_SINK_H
#define DEMO_HOST_DATA_SINK_H

#include <cstddef>
#include <cstdint>
#include <string>

/* wire v2 业务数据面回调。实现方不得保存传入指针，返回前必须复制或消费；
 * 回调发生在 Host poll 线程，不得销毁 host，不得持 queue/registry 锁。 */
struct HostSerialProfile {
    uint32_t frame_size = 0;
    uint16_t rows = 0;
    uint16_t cols = 0;
    uint32_t data_points = 0;
};

class IHostDataSink {
public:
    virtual ~IHostDataSink() = default;
    virtual void OnSessionOnline(const std::string &device_id,
                                 const std::string &session_id,
                                 const HostSerialProfile &profile) = 0;
    virtual void OnSerialBytes(const std::string &device_id,
                               const std::string &session_id,
                               const uint8_t *bytes, size_t length,
                               uint64_t receive_time_ms) = 0;
    virtual void OnSessionOffline(const std::string &device_id,
                                  const std::string &session_id,
                                  const std::string &reason) = 0;
};

#endif
