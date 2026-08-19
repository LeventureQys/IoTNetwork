#include "host_window.h"
#include "host_app.h"
#include "log_model.h"
#include "handshake_flow_widget.h"
#include "log.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
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

HostWindow::HostWindow(HostApp *host, const char *target_ssid, const char *target_password,
                       std::function<void()> provision, QWidget *parent)
    : QMainWindow(parent), host_(host), provision_(std::move(provision))
{
    setWindowTitle(QStringLiteral("TactileSense 上位机配网管理软件"));
    resize(1180, 900);

    log_model_ = new LogModel(this);
    g_host_log_model = log_model_;
    flow_widget_ = new HandshakeFlowWidget(HandshakeFlowWidget::Side::Host, this);
    g_host_flow_widget = flow_widget_;
    log_set_sink(HostLogSink);

    target_ssid_ = new QComboBox(this);
    target_ssid_->setEditable(true);
    target_ssid_->addItem(QString::fromUtf8(target_ssid));
    target_password_ = new QLineEdit(QString::fromUtf8(target_password), this);
    target_password_->setEchoMode(QLineEdit::Password);
    target_password_->setPlaceholderText(QStringLiteral("将下发给设备的 WiFi 密码（至少 8 位）"));
    wifi_scan_button_ = new QPushButton(QStringLiteral("扫描可下发的 WiFi"), this);
    connect(wifi_scan_button_, &QPushButton::clicked, this, &HostWindow::ScanTargetWifi);

    QHBoxLayout *wifi_row = new QHBoxLayout;
    wifi_row->addWidget(new QLabel(QStringLiteral("设备要连接的 WiFi："), this));
    wifi_row->addWidget(target_ssid_, 2);
    wifi_row->addWidget(new QLabel(QStringLiteral("下发密码："), this));
    wifi_row->addWidget(target_password_, 2);
    wifi_row->addWidget(wifi_scan_button_);

    summary_ = new QLabel(QStringLiteral("尚无设备注册"), this);
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

    provision_button_ = new QPushButton(QStringLiteral("连接设备 AP 并下发 WiFi 配置"), this);
    QPushButton *stop_button = new QPushButton(QStringLiteral("停止上位机（发送 host_bye）"), this);
    connect(provision_button_, &QPushButton::clicked, this, &HostWindow::StartProvision);
    connect(stop_button, &QPushButton::clicked, this, &QWidget::close);

    msg_edit_ = new QLineEdit(this);
    msg_edit_->setPlaceholderText(QStringLiteral("输入发给所选设备的联调消息（≤512 字节）"));
    msg_send_button_ = new QPushButton(QStringLiteral("发送消息"), this);
    connect(msg_send_button_, &QPushButton::clicked, this, &HostWindow::SendAppData);

    QHBoxLayout *buttons = new QHBoxLayout;
    buttons->addWidget(provision_button_);
    buttons->addWidget(stop_button);
    buttons->addWidget(msg_edit_, 1);
    buttons->addWidget(msg_send_button_);
    buttons->addStretch();
    QVBoxLayout *layout = new QVBoxLayout;
    layout->addLayout(wifi_row);
    layout->addWidget(summary_);
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

HostWindow::~HostWindow()
{
    log_set_sink(nullptr);
    g_host_log_model = nullptr;
    g_host_flow_widget = nullptr;
}

void HostWindow::Refresh()
{
    log_model_->Drain();
    flow_widget_->DrainEvents();
    log_view_->scrollToBottom();
    const auto entries = host_->registry().Snapshot();
    summary_->setText(QStringLiteral("已注册 %1 台设备，在线 %2 台")
                          .arg(entries.size()).arg(host_->OnlineCount()));
    flow_widget_->SetHostState(host_->Provisioning(), host_->ProvisionDone(),
                               host_->ProvisionTotal(), (int)entries.size(),
                               (int)host_->OnlineCount());
    device_table_->setRowCount((int)entries.size());
    for (int row = 0; row < (int)entries.size(); ++row) {
        const auto &entry = entries[(size_t)row];
        const QStringList values = {QString::fromStdString(entry.id),
                                    entry.state == "online" ? QStringLiteral("在线") : QStringLiteral("离线"),
                                    QString::fromStdString(entry.session_id),
                                    QString::fromStdString(entry.fw_version),
                                    QString::number(entry.proto_ver),
                                    QString::number(entry.lost_ping_count),
                                    QString::number(entry.reconnect_count)};
        for (int column = 0; column < values.size(); ++column)
            device_table_->setItem(row, column, new QTableWidgetItem(values[column]));
    }
}

void HostWindow::StartProvision()
{
    const QString ssid = target_ssid_->currentText().trimmed();
    const QString password = target_password_->text();
    if (ssid.isEmpty() || password.toUtf8().size() < 8) {
        LOG_E("MAIN", "请选择要下发给设备的 WiFi，并输入至少 8 位密码");
        return;
    }
    const QMessageBox::StandardButton confirmation = QMessageBox::question(
        this, QStringLiteral("确认设备配网"),
        QStringLiteral("即将连接设备热点 Modu_XXXX，并把以下网络配置下发给设备：\n\n"
                       "WiFi：%1\n密码：已填写（不会写入日志）\n\n是否继续？")
            .arg(ssid),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirmation != QMessageBox::Yes)
        return;
    host_->SetTargetNetwork(ssid.toStdString(), password.toStdString());
    LOG_I("MAIN", "上位机界面：准备把 WiFi 配置下发给设备，SSID=%s",
          ssid.toUtf8().constData());
    if (provision_)
        provision_();
}

void HostWindow::SendAppData()
{
    const int row = device_table_->currentRow();
    if (row < 0) {
        LOG_W("MAIN", "请先在设备表中选择要发送消息的设备");
        return;
    }
    const auto entries = host_->registry().Snapshot();
    if (row >= (int)entries.size()) {
        LOG_W("MAIN", "请先在设备表中选择要发送消息的设备");
        return;
    }
    const auto &entry = entries[(size_t)row];
    if (entry.state != "online") {
        LOG_W("MAIN", "设备 %s 当前离线，无法发送消息", entry.id.c_str());
        return;
    }
    const QByteArray utf8 = msg_edit_->text().toUtf8();
    const std::string text(utf8.constData(), (size_t)utf8.size());
    const int rc = host_->SendAppDataToDevice(entry.id, text);
    if (rc == DEMO_OK)
        LOG_I("MAIN", "上位机界面：联调消息已提交发送（目标=%s）", entry.id.c_str());
    else
        LOG_W("MAIN", "发送失败：消息为空或超过 %d 字节", APP_DATA_TEXT_MAX);
}

void HostWindow::ScanTargetWifi()
{
    wifi_scan_button_->setEnabled(false);
    LOG_I("MAIN", "正在扫描可下发给设备的 WiFi");
    std::vector<std::string> networks;
    const int result = host_->ScanWifiNetworks(&networks);
    if (result != DEMO_OK) {
        LOG_E("MAIN", "扫描可下发 WiFi 失败，返回码=%d", result);
        wifi_scan_button_->setEnabled(true);
        return;
    }
    const QString current = target_ssid_->currentText();
    target_ssid_->clear();
    for (const std::string &network : networks)
        target_ssid_->addItem(QString::fromUtf8(network));
    const int current_index = target_ssid_->findText(current);
    if (current_index >= 0)
        target_ssid_->setCurrentIndex(current_index);
    else
        target_ssid_->setEditText(current);
    LOG_I("MAIN", "可下发 WiFi 扫描完成，共 %zu 个", networks.size());
    wifi_scan_button_->setEnabled(true);
}

void HostWindow::closeEvent(QCloseEvent *event)
{
    if (!stopping_) {
        stopping_ = true;
        emit StopRequested();
    }
    event->accept();
}
