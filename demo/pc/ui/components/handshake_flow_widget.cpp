#include "handshake_flow_widget.h"

#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>

namespace {

constexpr int kNodeCount = 10;

QColor StateColor(HandshakeFlowWidget::NodeState state)
{
    switch (state) {
    case HandshakeFlowWidget::NodeState::Active: return QColor("#2563eb");
    case HandshakeFlowWidget::NodeState::Success: return QColor("#16a34a");
    case HandshakeFlowWidget::NodeState::Failed: return QColor("#dc2626");
    case HandshakeFlowWidget::NodeState::Retrying: return QColor("#ea580c");
    case HandshakeFlowWidget::NodeState::Pending: return QColor("#64748b");
    }
    return QColor("#64748b");
}

QString StateSymbol(HandshakeFlowWidget::NodeState state)
{
    switch (state) {
    case HandshakeFlowWidget::NodeState::Active: return QStringLiteral("●");
    case HandshakeFlowWidget::NodeState::Success: return QStringLiteral("✓");
    case HandshakeFlowWidget::NodeState::Failed: return QStringLiteral("×");
    case HandshakeFlowWidget::NodeState::Retrying: return QStringLiteral("↻");
    case HandshakeFlowWidget::NodeState::Pending: return QString::number(0);
    }
    return QString();
}

} // namespace

HandshakeFlowWidget::HandshakeFlowWidget(Side side, QWidget *parent)
    : QWidget(parent), side_(side)
{
    nodes_ = {
        {QStringLiteral("设备启动"), QStringLiteral("BOOT")},
        {QStringLiteral("热点就绪"), QStringLiteral("SoftAP")},
        {QStringLiteral("配网连接"), QStringLiteral("TCP")},
        {QStringLiteral("PIN 认证"), QStringLiteral("auth")},
        {QStringLiteral("写入 WiFi"), QStringLiteral("wifi_config")},
        {QStringLiteral("发现主机"), QStringLiteral("mDNS / 组播")},
        {QStringLiteral("业务连接"), QStringLiteral("TCP")},
        {QStringLiteral("设备注册"), QStringLiteral("device_hello")},
        {QStringLiteral("会话确认"), QStringLiteral("host_ack")},
        {QStringLiteral("心跳在线"), QStringLiteral("ping / pong")}
    };
    Reset(side_ == Side::Host ? QStringLiteral("等待扫描设备热点")
                              : QStringLiteral("设备正在启动"));
    if (side_ == Side::Device)
        Activate(0, QStringLiteral("BOOT"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(QStringLiteral("流程节点由真实协议日志和状态快照实时驱动"));
}

void HandshakeFlowWidget::PushLog(const QString &message)
{
    QMutexLocker locker(&pending_mutex_);
    pending_logs_.append(message);
}

void HandshakeFlowWidget::DrainEvents()
{
    QStringList logs;
    {
        QMutexLocker locker(&pending_mutex_);
        logs.swap(pending_logs_);
    }
    for (const QString &message : logs)
        ApplyLog(message);
}

void HandshakeFlowWidget::SetDeviceState(int state)
{
    switch (state) {
    case 0:
        Activate(0, QStringLiteral("BOOT"));
        break;
    case 1:
        Complete(0, QStringLiteral("已读取 WiFi 凭据"));
        Activate(4, QStringLiteral("正在校验目标 WiFi"));
        SetBehavior(QStringLiteral("连接并核验保存的目标 WiFi，成功后才开始发现上位机"),
                    QStringLiteral("STA / SSID 校验"));
        break;
    case 2:
        Complete(0, QStringLiteral("启动完成"));
        Activate(1, QStringLiteral("等待配网"));
        break;
    case 3:
        MarkThrough(4);
        Activate(5, QStringLiteral("搜索上位机"));
        break;
    case 4:
        MarkThrough(5);
        Activate(6, QStringLiteral("建立 TCP"));
        break;
    case 6:
        for (int index = 6; index < kNodeCount; ++index) {
            if (nodes_[index].state == NodeState::Active)
                nodes_[index].state = NodeState::Retrying;
        }
        SetBehavior(QStringLiteral("连接异常，正在退避重连或重新发现"), QStringLiteral("自愈 HEAL"));
        update();
        break;
    default:
        break;
    }
}

void HandshakeFlowWidget::SetHostState(bool provisioning, int done, int total,
                                       int registered, int online)
{
    if (provisioning) {
        SetBehavior(QStringLiteral("正在批量配网：%1 / %2").arg(done).arg(total),
                    QStringLiteral("SoftAP 配网"));
    } else if (online > 0) {
        MarkThrough(8);
        Complete(9, QStringLiteral("%1 台在线").arg(online));
        heartbeat_seen_ = true;
        SetBehavior(QStringLiteral("连接稳定，正在维护 %1 台设备会话").arg(online),
                    QStringLiteral("ping ⇄ pong"));
    } else if (registered > 0) {
        Retry(9, QStringLiteral("全部离线"));
        SetBehavior(QStringLiteral("设备已离线，等待自动重连"), QStringLiteral("TCP / 心跳"));
    }
}

void HandshakeFlowWidget::ApplyLog(const QString &message)
{
    if (message.contains(QStringLiteral("开始扫描并配网"))) {
        Reset(QStringLiteral("正在扫描 Modu_ 设备热点"));
        Activate(1, QStringLiteral("扫描热点"));
    } else if (message.contains(QStringLiteral("设备热点已启动"))) {
        Complete(0, QStringLiteral("启动完成"));
        Complete(1, QStringLiteral("SoftAP 可用"));
        Activate(2, QStringLiteral("等待客户端"));
        SetBehavior(QStringLiteral("配网热点已发布，等待上位机连接"), QStringLiteral("SoftAP"));
    } else if (message.contains(QStringLiteral("已连接设备热点"))) {
        MarkThrough(1);
        Complete(2, QStringLiteral("TCP 已连接"));
        Activate(3, QStringLiteral("发送 auth"));
        SetBehavior(QStringLiteral("已连接设备配网服务，准备校验一次性 PIN"), QStringLiteral("PC → Device"));
    } else if (message.contains(QStringLiteral("RX auth")) ||
               message.contains(QStringLiteral("已发送 auth"))) {
        MarkThrough(2);
        Activate(3, QStringLiteral("校验 PIN"));
        SetBehavior(QStringLiteral("正在执行设备身份认证"), QStringLiteral("auth → auth_result"));
    } else if (message.contains(QStringLiteral("auth 成功"))) {
        Complete(3, QStringLiteral("认证通过"));
        Activate(4, QStringLiteral("等待 WiFi 参数"));
    } else if (message.contains(QStringLiteral("auth 被设备拒绝")) ||
               message.contains(QStringLiteral("auth 失败")) ||
               message.contains(QStringLiteral("auth_result 超时"))) {
        Fail(3, QStringLiteral("认证失败"));
        SetBehavior(QStringLiteral("PIN 认证失败，可重新发起配网"), QStringLiteral("auth_result: fail"));
    } else if (message.contains(QStringLiteral("RX wifi_config")) ||
               message.contains(QStringLiteral("已发送 wifi_config"))) {
        Complete(3, QStringLiteral("认证通过"));
        Activate(4, QStringLiteral("连接目标 WiFi"));
        SetBehavior(QStringLiteral("正在写入并验证目标 WiFi 凭据"), QStringLiteral("wifi_config"));
    } else if (message.contains(QStringLiteral("配网成功"))) {
        Complete(4, QStringLiteral("WiFi 已验证"));
        Activate(5, QStringLiteral("等待关闭热点"));
        SetBehavior(QStringLiteral("网络凭据写入成功，准备切换到服务发现"), QStringLiteral("wifi_result: ok"));
    } else if (message.contains(QStringLiteral("配网失败")) ||
               message.contains(QStringLiteral("wifi_result 超时")) ||
               message.contains(QStringLiteral("wifi_config 参数无效"))) {
        Fail(4, QStringLiteral("写入失败"));
        SetBehavior(QStringLiteral("目标 WiFi 配置失败，请检查凭据或信号"), QStringLiteral("wifi_result: fail"));
    } else if (message.contains(QStringLiteral("close_ap")) ||
               message.contains(QStringLiteral("状态切换 -> discovery"))) {
        MarkThrough(4);
        Activate(5, QStringLiteral("搜索上位机"));
        SetBehavior(QStringLiteral("SoftAP 已关闭，正在发现局域网上位机"), QStringLiteral("mDNS / host_announce"));
    } else if (message.contains(QStringLiteral("host_announce 指向"))) {
        Complete(5, QStringLiteral("UDP 组播"));
        Activate(6, QStringLiteral("建立 TCP"));
        SetBehavior(QStringLiteral("已通过组播发现上位机，正在建立业务连接"), QStringLiteral("Device → Host TCP"));
    } else if (message.contains(QStringLiteral("通过 mDNS 发现"))) {
        Complete(5, QStringLiteral("mDNS"));
        Activate(6, QStringLiteral("建立 TCP"));
    } else if (message.contains(QStringLiteral("发现上位机："))) {
        Complete(5, QStringLiteral("地址已确认"));
        Activate(6, QStringLiteral("建立 TCP"));
    } else if (message.contains(QStringLiteral("已接受连接"))) {
        MarkThrough(5);
        Complete(6, QStringLiteral("TCP 已连接"));
        Activate(7, QStringLiteral("等待 hello"));
        SetBehavior(QStringLiteral("业务连接建立，等待设备提交身份与能力"), QStringLiteral("device_hello →"));
    } else if (message.contains(QStringLiteral("已发送 device_hello"))) {
        MarkThrough(6);
        Activate(7, QStringLiteral("已发送"));
        SetBehavior(QStringLiteral("设备信息已发送，等待上位机注册确认"), QStringLiteral("device_hello →"));
    } else if (message.contains(QStringLiteral("设备注册成功"))) {
        Complete(7, QStringLiteral("注册完成"));
        Complete(8, QStringLiteral("ACK 已发送"));
        Activate(9, QStringLiteral("等待心跳"));
        SetBehavior(QStringLiteral("注册成功，等待首个心跳确认链路稳定"), QStringLiteral("← host_ack: ok"));
    } else if (message.contains(QStringLiteral("host_ack 成功"))) {
        Complete(7, QStringLiteral("已注册"));
        Complete(8, QStringLiteral("会话已确认"));
        Activate(9, QStringLiteral("验证心跳"));
        SetBehavior(QStringLiteral("会话参数协商完成，开始心跳保活"), QStringLiteral("ping ⇄ pong"));
    } else if (message.contains(QStringLiteral("上位机繁忙")) ||
               message.contains(QStringLiteral("服务繁忙"))) {
        Retry(8, QStringLiteral("服务繁忙"));
        SetBehavior(QStringLiteral("上位机繁忙，设备进入长退避后重试"), QStringLiteral("host_ack: busy"));
    } else if (message.contains(QStringLiteral("host_ack 失败")) ||
               message.contains(QStringLiteral("不受支持，已拒绝"))) {
        Fail(8, QStringLiteral("协商失败"));
        SetBehavior(QStringLiteral("会话协商失败，请检查协议版本"), QStringLiteral("host_ack: fail"));
    } else if (message.contains(QStringLiteral("收到 pong")) ||
               message.contains(QStringLiteral("RX pong")) ||
               message.contains(QStringLiteral("\"cmd\":\"ping\""))) {
        MarkThrough(8);
        Complete(9, QStringLiteral("链路健康"));
        heartbeat_seen_ = true;
        SetBehavior(QStringLiteral("连接稳定，心跳收发正常"), QStringLiteral("ping ⇄ pong"));
    } else if (message.contains(QStringLiteral("心跳超时")) ||
               message.contains(QStringLiteral("连接已断开")) ||
               message.contains(QStringLiteral("会话丢失"))) {
        Retry(9, QStringLiteral("连接中断"));
        SetBehavior(QStringLiteral("连接中断，设备将自动退避重连"), QStringLiteral("HEAL"));
    } else if (message.contains(QStringLiteral("重新连接上位机")) ||
               message.contains(QStringLiteral("会话已恢复"))) {
        MarkThrough(6);
        Activate(7, QStringLiteral("恢复会话"));
        SetBehavior(QStringLiteral("TCP 已重连，正在恢复原会话"), QStringLiteral("device_hello + session_id"));
    } else if (message.contains(QStringLiteral("重连超时"))) {
        Retry(5, QStringLiteral("重新发现"));
        SetBehavior(QStringLiteral("直接重连超时，返回服务发现阶段"), QStringLiteral("DISCOVERY"));
    }
    update();
}

void HandshakeFlowWidget::Reset(const QString &behavior)
{
    for (Node &node : nodes_) {
        node.state = NodeState::Pending;
    }
    nodes_[0].detail = QStringLiteral("BOOT");
    nodes_[1].detail = QStringLiteral("SoftAP");
    nodes_[2].detail = QStringLiteral("TCP");
    nodes_[3].detail = QStringLiteral("auth");
    nodes_[4].detail = QStringLiteral("wifi_config");
    nodes_[5].detail = QStringLiteral("mDNS / 组播");
    nodes_[6].detail = QStringLiteral("TCP");
    nodes_[7].detail = QStringLiteral("device_hello");
    nodes_[8].detail = QStringLiteral("host_ack");
    nodes_[9].detail = QStringLiteral("ping / pong");
    heartbeat_seen_ = false;
    SetBehavior(behavior);
}

void HandshakeFlowWidget::Activate(int index, const QString &detail)
{
    if (index < 0 || index >= nodes_.size()) return;
    if (nodes_[index].state != NodeState::Success)
        nodes_[index].state = NodeState::Active;
    if (!detail.isEmpty()) nodes_[index].detail = detail;
    update();
}

void HandshakeFlowWidget::Complete(int index, const QString &detail)
{
    if (index < 0 || index >= nodes_.size()) return;
    nodes_[index].state = NodeState::Success;
    if (!detail.isEmpty()) nodes_[index].detail = detail;
    update();
}

void HandshakeFlowWidget::Fail(int index, const QString &detail)
{
    if (index < 0 || index >= nodes_.size()) return;
    nodes_[index].state = NodeState::Failed;
    nodes_[index].detail = detail;
    update();
}

void HandshakeFlowWidget::Retry(int index, const QString &detail)
{
    if (index < 0 || index >= nodes_.size()) return;
    nodes_[index].state = NodeState::Retrying;
    nodes_[index].detail = detail;
    update();
}

void HandshakeFlowWidget::MarkThrough(int index)
{
    for (int current = 0; current <= index && current < nodes_.size(); ++current) {
        if (nodes_[current].state == NodeState::Pending ||
            nodes_[current].state == NodeState::Active ||
            nodes_[current].state == NodeState::Retrying)
            nodes_[current].state = NodeState::Success;
    }
}

void HandshakeFlowWidget::SetBehavior(const QString &behavior, const QString &protocol)
{
    behavior_ = behavior;
    protocol_ = protocol;
    update();
}

QSize HandshakeFlowWidget::minimumSizeHint() const
{
    return QSize(760, 250);
}

void HandshakeFlowWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor("#f8fafc"));

    const QRectF panel = QRectF(rect()).adjusted(1, 1, -1, -1);
    painter.setPen(QPen(QColor("#cbd5e1"), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(panel, 10, 10);

    QFont title_font = painter.font();
    title_font.setBold(true);
    title_font.setPointSize(11);
    painter.setFont(title_font);
    painter.setPen(QColor("#0f172a"));
    painter.drawText(QRectF(18, 10, width() - 36, 24),
                     side_ == Side::Host ? QStringLiteral("端到端握手与连接状态 · 上位机视角")
                                         : QStringLiteral("端到端握手与连接状态 · 设备视角"));

    QFont small_font = painter.font();
    small_font.setBold(false);
    small_font.setPointSize(8);
    painter.setFont(small_font);
    const QString legend = QStringLiteral("● 进行中    ✓ 已完成    × 失败    ↻ 重试 / 自愈");
    painter.setPen(QColor("#64748b"));
    painter.drawText(QRectF(18, 33, width() - 36, 18), legend);

    const int columns = 5;
    const qreal gap = 18.0;
    const qreal left = 18.0;
    const qreal node_width = (width() - left * 2 - gap * (columns - 1)) / columns;
    const qreal node_height = 61.0;
    const qreal first_y = 58.0;
    const qreal row_gap = 17.0;
    QVector<QRectF> boxes;
    boxes.reserve(nodes_.size());
    for (int index = 0; index < nodes_.size(); ++index) {
        const int row = index / columns;
        const int visual_column = row == 0 ? index % columns : columns - 1 - index % columns;
        boxes.append(QRectF(left + visual_column * (node_width + gap),
                            first_y + row * (node_height + row_gap), node_width, node_height));
    }

    painter.setPen(QPen(QColor("#94a3b8"), 2));
    for (int index = 0; index + 1 < boxes.size(); ++index) {
        QPointF start;
        QPointF end;
        if (index == 4) {
            start = QPointF(boxes[index].center().x(), boxes[index].bottom());
            end = QPointF(boxes[index + 1].center().x(), boxes[index + 1].top());
        } else {
            start = QPointF(boxes[index].right(), boxes[index].center().y());
            end = QPointF(boxes[index + 1].left(), boxes[index + 1].center().y());
            if (index >= 5) {
                start = QPointF(boxes[index].left(), boxes[index].center().y());
                end = QPointF(boxes[index + 1].right(), boxes[index + 1].center().y());
            }
        }
        painter.drawLine(start, end);
        QPainterPath arrow;
        const bool down = index == 4;
        if (down) {
            arrow.moveTo(end);
            arrow.lineTo(end + QPointF(-4, -7));
            arrow.lineTo(end + QPointF(4, -7));
        } else if (index < 5) {
            arrow.moveTo(end);
            arrow.lineTo(end + QPointF(-7, -4));
            arrow.lineTo(end + QPointF(-7, 4));
        } else {
            arrow.moveTo(end);
            arrow.lineTo(end + QPointF(7, -4));
            arrow.lineTo(end + QPointF(7, 4));
        }
        arrow.closeSubpath();
        painter.fillPath(arrow, QColor("#94a3b8"));
    }

    for (int index = 0; index < nodes_.size(); ++index) {
        const Node &node = nodes_[index];
        const QRectF box = boxes[index];
        const QColor color = StateColor(node.state);
        painter.setPen(QPen(color, node.state == NodeState::Active ? 2.5 : 1.5));
        painter.setBrush(node.state == NodeState::Pending ? QColor("#ffffff") : QColor(color.red(), color.green(), color.blue(), 18));
        painter.drawRoundedRect(box, 7, 7);

        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(box.left() + 17, box.top() + 18), 10, 10);
        painter.setPen(Qt::white);
        QFont symbol_font = painter.font();
        symbol_font.setBold(true);
        symbol_font.setPointSize(9);
        painter.setFont(symbol_font);
        const QString symbol = node.state == NodeState::Pending ? QString::number(index + 1)
                                                                 : StateSymbol(node.state);
        painter.drawText(QRectF(box.left() + 7, box.top() + 8, 20, 20), Qt::AlignCenter, symbol);

        QFont node_font = painter.font();
        node_font.setPointSize(9);
        node_font.setBold(true);
        painter.setFont(node_font);
        painter.setPen(QColor("#0f172a"));
        painter.drawText(QRectF(box.left() + 34, box.top() + 7, box.width() - 40, 22),
                         Qt::AlignVCenter, node.title);
        node_font.setPointSize(8);
        node_font.setBold(false);
        painter.setFont(node_font);
        painter.setPen(color);
        painter.drawText(QRectF(box.left() + 9, box.top() + 34, box.width() - 18, 18),
                         Qt::AlignCenter, painter.fontMetrics().elidedText(node.detail, Qt::ElideRight,
                                                                          int(box.width() - 18)));
    }

    const QRectF behavior_box(18, height() - 42, width() - 36, 29);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#e2e8f0"));
    painter.drawRoundedRect(behavior_box, 6, 6);
    painter.setPen(QColor("#334155"));
    QFont behavior_font = painter.font();
    behavior_font.setPointSize(9);
    behavior_font.setBold(true);
    painter.setFont(behavior_font);
    QString text = QStringLiteral("当前行为：%1").arg(behavior_);
    if (!protocol_.isEmpty())
        text += QStringLiteral("    协议：%1").arg(protocol_);
    painter.drawText(behavior_box.adjusted(10, 0, -10, 0), Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(text, Qt::ElideRight,
                                                      int(behavior_box.width() - 20)));
}
