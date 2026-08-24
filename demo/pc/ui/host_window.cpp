#include "host_window.h"
#include "host_app.h"
#include "log_model.h"
#include "handshake_flow_widget.h"
#include "serial_data_model.h"
#include "serial_text_frame.h"
#include "log.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

LogModel *g_host_log_model = nullptr;
HandshakeFlowWidget *g_host_flow_widget = nullptr;

void HostLogSink(int level, const char *module, const char *message)
{
    if (g_host_log_model)
        g_host_log_model->PushLog(level, QString::fromUtf8("[%1] %2").arg(module, message));
    if (g_host_flow_widget)
        g_host_flow_widget->PushLog(QString::fromUtf8(message));
}

} // namespace

HostWindow::HostWindow(HostApp *host, QWidget *parent)
    : QMainWindow(parent), host_(host)
{
    setWindowTitle(QStringLiteral("TactileSense 上位机管理软件"));
    resize(1180, 860);

    log_model_ = new LogModel(this);
    g_host_log_model = log_model_;
    flow_widget_ = new HandshakeFlowWidget(this);
    g_host_flow_widget = flow_widget_;
    log_set_sink(HostLogSink);

    serial_model_ = new SerialDataModel(this);
    {
        const std::string serial_log_dir = host_->params().log_dir;
        if (!serial_log_dir.empty())
            serial_model_->SetLogDirectory(serial_log_dir);
    }

    const QString ssid = QString::fromUtf8(host_->params().pc_ap_ssid);
    const QString ip = QString::fromUtf8(host_->params().pc_ap_ip);
    hotspot_status_ = new QLabel(
        QStringLiteral("热点：%1 · %2/%3 · 端口 %4 · 运行中")
            .arg(ssid, ip)
            .arg(host_->params().pc_ap_prefix_length)
            .arg(host_->params().host_tcp_port),
        this);
    hotspot_status_->setStyleSheet("QLabel { font-weight: bold; color: #0f172a; }");

    summary_ = new QLabel(QStringLiteral("尚无设备注册"), this);
    serial_stats_ = new QLabel(
        QStringLiteral("串口数据：等待设备 SERIAL_BYTES 会话"), this);
    serial_stats_->setStyleSheet("QLabel { color: #334155; }");
    device_table_ = new QTableWidget(this);
    device_table_->setColumnCount(7);
    device_table_->setHorizontalHeaderLabels({QStringLiteral("设备 ID"), QStringLiteral("状态"),
                                               QStringLiteral("会话"), QStringLiteral("固件"),
                                               QStringLiteral("协议"), QStringLiteral("丢失心跳"),
                                               QStringLiteral("重连次数")});
    device_table_->horizontalHeader()->setStretchLastSection(true);
    device_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    log_view_ = new QListView(this);
    log_view_->setModel(log_model_);
    log_view_->setFont(QFont(QStringLiteral("Consolas"), 9));
    log_view_->setStyleSheet("QListView { background:#1e1e1e; color:#d4d4d4; }");

    QSplitter *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(device_table_);
    splitter->addWidget(log_view_);
    splitter->setStretchFactor(1, 2);

    QPushButton *stop_button = new QPushButton(QStringLiteral("停止服务并关闭热点"), this);
    connect(stop_button, &QPushButton::clicked, this, &QWidget::close);

    msg_edit_ = new QLineEdit(this);
    msg_edit_->setPlaceholderText(QStringLiteral("输入发给设备的联调消息（≤512 字节）"));
    msg_send_button_ = new QPushButton(QStringLiteral("发送消息"), this);
    connect(msg_send_button_, &QPushButton::clicked, this, &HostWindow::SendAppData);

    serial_edit_ = new QLineEdit(this);
    serial_edit_->setPlaceholderText(
        QStringLiteral("输入单帧串口文本（UTF-8，≤512 字节），按 AA 55 01 规则编码"));
    serial_send_button_ = new QPushButton(QStringLiteral("发送串口单帧"), this);
    connect(serial_send_button_, &QPushButton::clicked, this,
            &HostWindow::SendSerialFrame);

    QHBoxLayout *serial_buttons = new QHBoxLayout;
    serial_buttons->addWidget(new QLabel(QStringLiteral("串口单帧："), this));
    serial_buttons->addWidget(serial_edit_, 1);
    serial_buttons->addWidget(serial_send_button_);
    serial_buttons->addStretch();

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addWidget(stop_button);
    buttons->addWidget(msg_edit_, 1);
    buttons->addWidget(msg_send_button_);
    buttons->addStretch();
    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(hotspot_status_);
    layout->addWidget(summary_);
    layout->addWidget(serial_stats_);
    layout->addLayout(serial_buttons);
    layout->addWidget(flow_widget_);
    layout->addWidget(splitter, 1);
    layout->addLayout(buttons);
    QWidget *central = new QWidget(this);
    central->setLayout(layout);
    setCentralWidget(central);

    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &HostWindow::Refresh);
    timer->start(150);
    LOG_I("MAIN", "上位机界面已就绪");
}

IHostDataSink *HostWindow::serial_model() const
{
    return serial_model_;
}

HostWindow::~HostWindow()
{
    /* 先反注册数据面 sink，再释放 SerialDataModel，避免 host 线程回调悬空指针 */
    host_->SetDataSink(nullptr);
    delete serial_model_;
    serial_model_ = nullptr;
    log_set_sink(nullptr);
    g_host_log_model = nullptr;
    g_host_flow_widget = nullptr;
}

void HostWindow::Refresh()
{
    log_model_->Drain();
    flow_widget_->DrainEvents();
    serial_model_->Drain();
    serial_stats_->setText(serial_model_->DisplayText());
    log_view_->scrollToBottom();
    const auto entries = host_->registry().Snapshot();
    summary_->setText(QStringLiteral("已注册 %1 台设备，在线 %2 台")
                          .arg(entries.size()).arg(host_->OnlineCount()));
    flow_widget_->SetHostState((int)entries.size(), (int)host_->OnlineCount());

    /* 设备表固定最多一行：优先展示在线设备 */
    const DeviceEntry *shown = nullptr;
    for (const auto &entry : entries) {
        if (entry.state == "online") {
            shown = &entry;
            break;
        }
    }
    if (!shown && !entries.empty())
        shown = &entries.back();

    device_table_->setRowCount(shown ? 1 : 0);
    if (shown) {
        const QStringList values = {QString::fromStdString(shown->id),
                                    shown->state == "online" ? QStringLiteral("在线")
                                                             : QStringLiteral("离线"),
                                    QString::fromStdString(shown->session_id),
                                    QString::fromStdString(shown->fw_version),
                                    QString::number(shown->proto_ver),
                                    QString::number(shown->lost_ping_count),
                                    QString::number(shown->reconnect_count)};
        for (int column = 0; column < values.size(); ++column)
            device_table_->setItem(0, column, new QTableWidgetItem(values[column]));
    }
}

void HostWindow::SendAppData()
{
    const auto entries = host_->registry().Snapshot();
    const DeviceEntry *online = nullptr;
    for (const auto &entry : entries) {
        if (entry.state == "online") {
            online = &entry;
            break;
        }
    }
    if (!online) {
        LOG_W("MAIN", "当前无在线设备，无法发送消息");
        return;
    }
    const QByteArray utf8 = msg_edit_->text().toUtf8();
    const std::string text(utf8.constData(), (size_t)utf8.size());
    const int rc = host_->SendAppDataToDevice(online->id, text);
    if (rc == DEMO_OK)
        LOG_I("MAIN", "上位机界面：联调消息已提交发送（目标=%s）", online->id.c_str());
    else
        LOG_W("MAIN", "发送失败：消息为空或超过 %d 字节", APP_DATA_TEXT_MAX);
}

void HostWindow::SendSerialFrame()
{
    const auto entries = host_->registry().Snapshot();
    const DeviceEntry *online = nullptr;
    for (const auto &entry : entries) {
        if (entry.state == "online") {
            online = &entry;
            break;
        }
    }
    if (!online) {
        LOG_W("MAIN", "当前无在线设备，无法发送串口单帧");
        return;
    }

    const QByteArray utf8 = serial_edit_->text().toUtf8();
    const std::string text(utf8.constData(), static_cast<size_t>(utf8.size()));
    std::vector<uint8_t> frame;
    if (!serial_text_frame::Encode(text, &frame)) {
        LOG_W("MAIN", "串口单帧编码失败：文本为空或超过 %zu 字节",
              static_cast<size_t>(serial_text_frame::kMaxPayloadBytes));
        return;
    }

    const int rc = host_->SendSerialFrameToDevice(online->id, frame);
    if (rc == DEMO_OK) {
        LOG_I("MAIN", "串口单帧已编码并提交发送（目标=%s，%zu 字节）",
              online->id.c_str(), frame.size());
        serial_edit_->clear();
    } else {
        LOG_W("MAIN", "串口单帧提交失败（rc=%d）", rc);
    }
}

void HostWindow::closeEvent(QCloseEvent *event)
{
    if (!stopping_) {
        stopping_ = true;
        emit StopRequested();
    }
    event->accept();
}
