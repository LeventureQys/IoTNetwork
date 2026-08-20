#include "serial_data_model.h"

#include "log.h"

#include <QMutexLocker>

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace {

std::string SanitizeFileNameComponent(const std::string &value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_')
            out += static_cast<char>(ch);
        else
            out += '_';
    }
    if (out.empty())
        out = "device";
    return out;
}

uint64_t WallClockNowMs()
{
    const auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count());
}

std::string SessionTimeStamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &now) != 0)
        return "unknown";
#else
    if (localtime_r(&now, &tm) == nullptr)
        return "unknown";
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

QString LastReceiveText(uint64_t wall_ms)
{
    if (wall_ms == 0)
        return QStringLiteral("尚未接收");
    const std::time_t seconds = static_cast<std::time_t>(wall_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &seconds) != 0)
        return QStringLiteral("--:--:--");
#else
    if (localtime_r(&seconds, &tm) == nullptr)
        return QStringLiteral("--:--:--");
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return QString::fromUtf8(buf);
}

} // namespace

SerialDataModel::SerialDataModel(QObject *parent)
    : QObject(parent)
{
}

SerialDataModel::~SerialDataModel()
{
    QMutexLocker locker(&mu_);
    CloseFileLocked();
}

void SerialDataModel::SetLogDirectory(const std::string &dir)
{
    QMutexLocker locker(&mu_);
    log_dir_ = dir.empty() ? "logs" : dir;
}

void SerialDataModel::OnSessionOnline(const std::string &device_id,
                                      const std::string &session_id,
                                      const HostSerialProfile &profile)
{
    QMutexLocker locker(&mu_);

    device_id_ = device_id;
    session_id_ = session_id;
    profile_ = profile;
    tail_.clear();

    /* 正常流程此时旧文件已关闭；防御 HostTcpServer 关停路径未回调 Offline。 */
    CloseFileLocked();
    if (!OpenFileLocked()) {
        LOG_W("SERIAL", "设备 %s 的串口落盘文件创建失败（目录=%s）",
              device_id_.c_str(), log_dir_.c_str());
        file_path_.clear();
        return;
    }
    LOG_I("SERIAL", "设备 %s 上线，串口落盘文件：%s（frame_size=%u）",
          device_id_.c_str(), file_path_.c_str(),
          static_cast<unsigned>(profile_.frame_size));
}

void SerialDataModel::OnSerialBytes(const std::string &device_id,
                                    const std::string &session_id,
                                    const uint8_t *bytes, size_t length,
                                    uint64_t receive_time_ms)
{
    QMutexLocker locker(&mu_);

    device_id_ = device_id;
    session_id_ = session_id;
    if (length == 0)
        return;

    last_rx_ms_ = receive_time_ms;
    last_rx_wall_ms_ = WallClockNowMs();
    total_bytes_ += length;

    if (bytes == nullptr) {
        dropped_bytes_ += length;
        return;
    }

    if (file_ != nullptr) {
        const size_t written = fwrite(bytes, 1, length, file_);
        if (written != length) {
            dropped_bytes_ += length - written;
            LOG_W("SERIAL", "串口数据落盘写入不完整：期望 %zu 字节，实际 %zu 字节",
                  length, written);
        }
    } else {
        dropped_bytes_ += length;
    }

    /* 按 profile_.frame_size 切帧；不足一帧的残留与下一批数据拼接。 */
    const size_t frame_size = static_cast<size_t>(profile_.frame_size);
    if (frame_size == 0) {
        tail_.clear();
        frame_count_ += 1;
        return;
    }

    if (tail_.empty()) {
        frame_count_ += length / frame_size;
        const size_t remain = length % frame_size;
        if (remain != 0)
            tail_.assign(bytes + length - remain, bytes + length);
        return;
    }

    std::vector<uint8_t> combined;
    combined.reserve(tail_.size() + length);
    combined.insert(combined.end(), tail_.begin(), tail_.end());
    combined.insert(combined.end(), bytes, bytes + length);
    frame_count_ += combined.size() / frame_size;
    const size_t remain = combined.size() % frame_size;
    tail_.assign(remain == 0 ? combined.end() : combined.end() - remain,
                 combined.end());
}

void SerialDataModel::OnSessionOffline(const std::string &device_id,
                                       const std::string &session_id,
                                       const std::string &reason)
{
    QMutexLocker locker(&mu_);

    if (file_path_.empty()) {
        LOG_I("SERIAL", "设备 %s 会话离线（%s）：无串口落盘文件需要关闭",
              device_id.c_str(), reason.c_str());
        return;
    }

    CloseFileLocked();
    LOG_I("SERIAL", "设备 %s（会话 %s）离线，串口落盘文件已关闭：%s；"
          "累计 %llu 字节 / %llu 帧",
          device_id.c_str(), session_id.c_str(), file_path_.c_str(),
          static_cast<unsigned long long>(total_bytes_),
          static_cast<unsigned long long>(frame_count_));
}

void SerialDataModel::Drain()
{
    QMutexLocker locker(&mu_);
    display_.total_bytes = total_bytes_;
    display_.frame_count = frame_count_;
    display_.last_rx_wall_ms = last_rx_wall_ms_;
    display_.dropped_bytes = dropped_bytes_;
    display_.device_id = device_id_;
    display_.session_id = session_id_;
    display_.file_path = file_path_;
    display_.file_open = file_ != nullptr;
}

QString SerialDataModel::DisplayText() const
{
    const Stats &s = display_;
    if (!s.file_open && s.file_path.empty() && s.device_id.empty() &&
        s.total_bytes == 0)
        return QStringLiteral("串口数据：等待设备 SERIAL_BYTES 会话");

    QString text = QStringLiteral("串口数据：已接收 %1 字节 / %2 帧，最近接收 %3")
                       .arg(QString::number(s.total_bytes),
                            QString::number(s.frame_count),
                            LastReceiveText(s.last_rx_wall_ms));
    if (s.file_open && !s.file_path.empty()) {
        text += QStringLiteral("，文件：");
        text += QString::fromStdString(s.file_path);
    } else if (!s.file_path.empty()) {
        text += QStringLiteral("，最近文件：");
        text += QString::fromStdString(s.file_path);
        text += QStringLiteral("（已关闭）");
        if (s.dropped_bytes != 0) {
            text += QStringLiteral("，丢弃 %1 字节")
                        .arg(QString::number(s.dropped_bytes));
        }
    } else {
        text += QStringLiteral("，落盘不可用，丢弃 %1 字节")
                    .arg(QString::number(s.dropped_bytes));
    }
    return text;
}

void SerialDataModel::CloseFileLocked()
{
    if (file_ == nullptr)
        return;
    if (fflush(file_) != 0)
        LOG_W("SERIAL", "串口落盘文件 flush 失败：%s", file_path_.c_str());
    if (fclose(file_) != 0)
        LOG_W("SERIAL", "串口落盘文件关闭失败：%s", file_path_.c_str());
    file_ = nullptr;
}

bool SerialDataModel::OpenFileLocked()
{
    const std::string dir = log_dir_.empty() ? "logs" : log_dir_;
    const std::filesystem::path dir_path(dir);
    std::error_code ec;
    std::filesystem::create_directories(dir_path, ec);
    if (ec) {
        LOG_W("SERIAL", "创建串口落盘目录失败：%s（%s）", dir.c_str(),
              ec.message().c_str());
        return false;
    }

    const std::string stamp = SessionTimeStamp();
    const std::filesystem::path base_path =
        dir_path / ("serial_" + SanitizeFileNameComponent(device_id_) + "_" +
                    stamp);

    bool found = false;
    std::filesystem::path candidate;
    for (int suffix = 0; suffix < 10000; ++suffix) {
        candidate = base_path;
        if (suffix > 0)
            candidate += "_" + std::to_string(suffix);
        candidate += ".bin";

        const bool exists = std::filesystem::exists(candidate, ec);
        if (ec)
            break;
        if (!exists) {
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_W("SERIAL", "无法为设备 %s 生成唯一的串口落盘文件名",
              device_id_.c_str());
        return false;
    }

    file_path_ = candidate.string();
#ifdef _WIN32
    file_ = _wfopen(candidate.c_str(), L"wb");
#else
    file_ = fopen(candidate.c_str(), "wb");
#endif
    if (file_ == nullptr) {
        LOG_W("SERIAL", "打开串口落盘文件失败：%s", file_path_.c_str());
        return false;
    }
    return true;
}
