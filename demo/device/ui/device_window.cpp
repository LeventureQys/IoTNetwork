#include "device_window.h"
#include "device_host.h"
#include "log_model.h"
#include "flow_widget.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString StateName(int state)
{
    switch (state) {
    case 0: return QStringLiteral("启动");
    case 1: return QStringLiteral("连接目标 WiFi");
    case 2: return QStringLiteral("等待上位机配网");
    case 3: return QStringLiteral("发现上位机");
    case 4: return QStringLiteral("连接上位机");
    case 5: return QStringLiteral("会话在线");
    case 6: return QStringLiteral("异常自愈");
    default: return QStringLiteral("未知");
    }
}

} // namespace

DeviceWindow::DeviceWindow(device_host_t *host, QWidget *parent)
    : QMainWindow(parent), host_(host)
{
    setWindowTitle(QStringLiteral("TactileSense 下位机设备模拟器"));
    resize(1050, 860);

    log_model_ = new LogModel(this);
    flow_widget_ = new FlowWidget(this);

    state_label_ = new QLabel(this);
    ap_label_ = new QLabel(this);
    session_label_ = new QLabel(this);
    error_label_ = new QLabel(this);
    error_label_->setObjectName(QStringLiteral("error_label"));
    error_label_->setWordWrap(true);
    error_label_->setStyleSheet("QLabel { color:#c62828; }");
    error_label_->hide();

    QFormLayout *status = new QFormLayout;
    status->addRow(QStringLiteral("当前状态："), state_label_);
    status->addRow(QStringLiteral("配网热点："), ap_label_);
    status->addRow(QStringLiteral("运行信息："), session_label_);
    status->addRow(QStringLiteral("错误："), error_label_);

    log_view_ = new QListView(this);
    log_view_->setModel(log_model_);
    log_view_->setFont(QFont(QStringLiteral("Consolas"), 9));
    log_view_->setStyleSheet("QListView { background:#1e1e1e; color:#d4d4d4; }");

    action_box_ = new QComboBox(this);
    action_box_->setObjectName(QStringLiteral("action_box"));
    action_box_->addItem(QStringLiteral("断开目标 WiFi"), QStringLiteral("wifi_disconnect"));
    action_box_->addItem(QStringLiteral("恢复目标 WiFi"), QStringLiteral("wifi_ok"));
    action_box_->addItem(QStringLiteral("WiFi 认证失败"), QStringLiteral("wifi_auth_fail"));
    action_box_->addItem(QStringLiteral("设置 RSSI"), QStringLiteral("rssi_set"));
    action_box_->addItem(QStringLiteral("屏蔽组播"), QStringLiteral("mcast_block"));
    action_box_->addItem(QStringLiteral("恢复组播"), QStringLiteral("mcast_unblock"));
    action_box_->addItem(QStringLiteral("突发发送"), QStringLiteral("burst_send"));
    argument_edit_ = new QLineEdit(this);
    argument_edit_->setObjectName(QStringLiteral("argument_edit"));
    argument_edit_->setPlaceholderText(QStringLiteral("参数，例如 -80"));
    QPushButton *inject_button = new QPushButton(QStringLiteral("注入故障"), this);
    QPushButton *stop_button = new QPushButton(QStringLiteral("停止下位机"), this);
    connect(inject_button, &QPushButton::clicked, this, &DeviceWindow::Inject);
    connect(stop_button, &QPushButton::clicked, this, &QWidget::close);

    msg_edit_ = new QLineEdit(this);
    msg_edit_->setObjectName(QStringLiteral("msg_edit"));
    msg_edit_->setPlaceholderText(QStringLiteral("输入发给上位机的联调消息（≤512 字节）"));
    msg_send_button_ = new QPushButton(QStringLiteral("发送消息"), this);
    connect(msg_send_button_, &QPushButton::clicked, this, &DeviceWindow::SendAppData);

    QHBoxLayout *controls = new QHBoxLayout;
    controls->addWidget(action_box_);
    controls->addWidget(argument_edit_);
    controls->addWidget(inject_button);
    controls->addWidget(msg_edit_, 1);
    controls->addWidget(msg_send_button_);
    controls->addWidget(stop_button);

    QVBoxLayout *layout = new QVBoxLayout;
    layout->addLayout(status);
    layout->addWidget(flow_widget_);
    layout->addWidget(log_view_, 1);
    layout->addLayout(controls);
    QWidget *central = new QWidget(this);
    central->setLayout(layout);
    setCentralWidget(central);

    QTimer *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &DeviceWindow::Refresh);
    timer->start(150);
}

DeviceWindow::~DeviceWindow() = default;

void DeviceWindow::ShowError(const QString &message)
{
    error_label_->setText(message);
    error_label_->show();
}

void DeviceWindow::Refresh()
{
    if (!host_)
        return;

    device_log_record_t records[64];
    size_t count = 0;
    uint64_t dropped = 0;
    if (device_host_drain_logs(host_, records, 64, &count, &dropped) == DEVICE_OK) {
        for (size_t i = 0; i < count; ++i) {
            log_model_->Append(QString::fromUtf8("[%1] %2")
                                   .arg(QString::fromUtf8(records[i].module),
                                        QString::fromUtf8(records[i].message)));
        }
        if (dropped > 0)
            log_model_->Append(
                QStringLiteral("[队列] 已丢弃 %1 条日志").arg(dropped));
    }
    log_view_->scrollToBottom();

    device_snapshot_t snap;
    if (device_host_get_snapshot(host_, &snap) != DEVICE_OK) {
        ShowError(QStringLiteral("快照读取失败"));
        return;
    }
    error_label_->setVisible(false);
    state_label_->setText(StateName(snap.device_state));
    ap_label_->setText(QStringLiteral("SSID=%1，密码=%2，PIN=%3，端口=%4")
                           .arg(QString::fromUtf8(snap.ap_ssid),
                                QString::fromUtf8(snap.ap_password),
                                QString::fromUtf8(snap.provision_pin))
                           .arg(snap.provision_port));
    session_label_->setText(
        QStringLiteral("RSSI=%1 dBm，运行=%2 秒，后端=%3")
            .arg(snap.rssi_dbm)
            .arg(snap.uptime_seconds)
            .arg(QString::fromUtf8(snap.backend_name)));
    flow_widget_->SetDeviceState(snap.device_state);
    flow_widget_->SetOnline(snap.session_online != 0);
    if (snap.last_error[0] != '\0')
        ShowError(QString::fromUtf8(snap.last_error));
}

void DeviceWindow::Inject()
{
    const QByteArray action = action_box_->currentData().toString().toUtf8();
    const QByteArray argument = argument_edit_->text().trimmed().toUtf8();
    device_error_t err;
    device_result_t rc = device_host_inject_fault(
        host_, action.constData(), argument.isEmpty() ? nullptr : argument.constData(), &err);
    if (rc != DEVICE_OK)
        ShowError(QStringLiteral("故障注入失败（%1）：%2")
                      .arg(static_cast<int>(rc))
                      .arg(QString::fromUtf8(err.message)));
    else
        ShowError(QStringLiteral("故障注入已提交：%1").arg(QString::fromUtf8(action)));
}

void DeviceWindow::SendAppData()
{
    const QByteArray text = msg_edit_->text().toUtf8();
    if (text.isEmpty())
        return;
    device_error_t err;
    device_result_t rc =
        device_host_send_app_data(host_, text.constData(), text.size(), &err);
    if (rc == DEVICE_OK)
        ShowError(QStringLiteral("消息已提交发送"));
    else
        ShowError(QStringLiteral("发送失败（%1）：%2")
                      .arg(static_cast<int>(rc))
                      .arg(QString::fromUtf8(err.message)));
}

void DeviceWindow::closeEvent(QCloseEvent *event)
{
    if (!stopping_) {
        stopping_ = true;
        emit StopRequested();
    }
    event->accept();
}
