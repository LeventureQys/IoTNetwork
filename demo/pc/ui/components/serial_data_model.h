#ifndef DEMO_SERIAL_DATA_MODEL_H
#define DEMO_SERIAL_DATA_MODEL_H

#include "host_data_sink.h"

#include <QMutex>
#include <QObject>
#include <QString>
#include <QWaitCondition>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>
#include <vector>

/* SERIAL_BYTES 数据面模型。
 * IHostDataSink 回调发生在 HostApp::Run 的 poll 线程：仅复制到有界内存队列并
 * 更新统计，绝不做磁盘 IO；独立 writer 线程负责落盘。Drain()/DisplayText()
 * 由 GUI 线程的 150ms 定时器调用，只搬运统计快照。 */
class SerialDataModel : public QObject, public IHostDataSink {
    Q_OBJECT
public:
    explicit SerialDataModel(QObject *parent = nullptr);
    ~SerialDataModel() override;

    /* 落盘根目录；须在 main 把本对象接线到 HostApp 之前设置。 */
    void SetLogDirectory(const std::string &dir);

    /* GUI 线程调用：把 host 线程累计的统计复制到 display_，不阻塞数据接收。 */
    void Drain();
    /* GUI 线程调用：返回 Drain 后的统计标签文本。 */
    QString DisplayText() const;

    /* IHostDataSink（host poll 线程回调） */
    void OnSessionOnline(const std::string &device_id,
                         const std::string &session_id,
                         const HostSerialProfile &profile) override;
    void OnSerialBytes(const std::string &device_id,
                       const std::string &session_id,
                       const uint8_t *bytes, size_t length,
                       uint64_t receive_time_ms) override;
    void OnSessionOffline(const std::string &device_id,
                          const std::string &session_id,
                          const std::string &reason) override;

private:
    struct Stats {
        uint64_t total_bytes = 0;
        uint64_t frame_count = 0;
        uint64_t last_rx_wall_ms = 0;
        uint64_t dropped_bytes = 0;
        std::string device_id;
        std::string session_id;
        std::string file_path;
        bool file_open = false;
    };

    enum class WriteOpKind { Open, Data, Close };
    struct WriteOp {
        WriteOpKind kind = WriteOpKind::Data;
        std::string path;
        std::vector<uint8_t> bytes;
    };

    static constexpr size_t kWriteQueueCapacity = 4 * 1024 * 1024;

    void WriterLoop();
    void CloseFileLocked();
    bool OpenFileLocked();

    QMutex mu_;
    std::string log_dir_ = "logs";

    /* 以下字段由 host 线程写、Drain（GUI 线程）经 mu_ 读 */
    HostSerialProfile profile_;
    std::string device_id_;
    std::string session_id_;
    bool file_open_ = false;
    std::string file_path_;
    std::deque<WriteOp> write_queue_;
    size_t queued_bytes_ = 0;
    bool writer_stopping_ = false;
    QWaitCondition writer_cv_;
    std::thread writer_thread_;
    uint64_t total_bytes_ = 0;
    uint64_t frame_count_ = 0;        /* 按 profile_.frame_size 切帧计数 */
    uint64_t last_rx_ms_ = 0;         /* 协议传入的 receive_time_ms（单调时钟） */
    uint64_t last_rx_wall_ms_ = 0;    /* 最近一次接收的墙上时钟，用于 UI 显示 */
    uint64_t dropped_bytes_ = 0;      /* 文件不可写等异常时计数 */
    std::vector<uint8_t> tail_;       /* 跨回调残留的不足一帧字节 */

    /* GUI 线程专属快照：Drain 写入、DisplayText 读取 */
    Stats display_;
};

#endif
