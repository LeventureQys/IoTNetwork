#include "handshake_flow_widget.h"

#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>

namespace {

constexpr int kNodeCount = 6;

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

HandshakeFlowWidget::HandshakeFlowWidget(QWidget *parent)
    : QWidget(parent)
{
    nodes_ = {
        {QStringLiteral("启动"), QStringLiteral("初始化")},
        {QStringLiteral("热点启动"), QStringLiteral("移动热点")},
        {QStringLiteral("固定 IP"), QStringLiteral("192.168.137.1/24")},
        {QStringLiteral("TCP 监听"), QStringLiteral("0.0.0.0:5935")},
        {QStringLiteral("设备握手"), QStringLiteral("device_hello")},
        {QStringLiteral("会话在线"), QStringLiteral("ping / pong")}
    };
    Reset(QStringLiteral("正在启动上位机"));
    Activate(0, QStringLiteral("启动"));
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

void HandshakeFlowWidget::SetHostState(int registered, int online)
{
    /* HostApp::Start() 成功后窗口才显示，因此前四步（启动/热点/固定IP/TCP）已就绪 */
    MarkThrough(3);
    if (online > 0) {
        Complete(4, QStringLiteral("已握手"));
        Complete(5, QStringLiteral("%1 台在线").arg(online));
        heartbeat_seen_ = true;
        SetBehavior(QStringLiteral("连接稳定，正在维护 %1 台设备会话").arg(online),
                    QStringLiteral("ping ⇄ pong"));
    } else if (registered > 0) {
        Retry(5, QStringLiteral("等待设备"));
        Activate(4, QStringLiteral("等待握手"));
        SetBehavior(QStringLiteral("设备已离线，等待自动重连"), QStringLiteral("TCP / 心跳"));
    } else {
        Activate(4, QStringLiteral("等待握手"));
        SetBehavior(QStringLiteral("服务已就绪，等待唯一设备连接"),
                    QStringLiteral("device_hello →"));
    }
}

void HandshakeFlowWidget::ApplyLog(const QString &message)
{
    if (message.contains(QStringLiteral("已接受连接"))) {
        Activate(4, QStringLiteral("等待 hello"));
        SetBehavior(QStringLiteral("业务连接建立，等待设备提交身份"), QStringLiteral("device_hello →"));
    } else if (message.contains(QStringLiteral("设备注册成功"))) {
        Complete(4, QStringLiteral("握手完成"));
        Activate(5, QStringLiteral("会话在线"));
        SetBehavior(QStringLiteral("注册成功，等待心跳确认链路稳定"), QStringLiteral("← host_ack: ok"));
    } else if (message.contains(QStringLiteral("收到 pong")) ||
               message.contains(QStringLiteral("RX pong")) ||
               message.contains(QStringLiteral("\"cmd\":\"ping\""))) {
        MarkThrough(4);
        Complete(5, QStringLiteral("链路健康"));
        heartbeat_seen_ = true;
        SetBehavior(QStringLiteral("连接稳定，心跳收发正常"), QStringLiteral("ping ⇄ pong"));
    } else if (message.contains(QStringLiteral("心跳超时")) ||
               message.contains(QStringLiteral("连接已断开")) ||
               message.contains(QStringLiteral("会话丢失"))) {
        Retry(5, QStringLiteral("连接中断"));
        SetBehavior(QStringLiteral("连接中断，等待设备自动重连"), QStringLiteral("HEAL"));
    } else if (message.contains(QStringLiteral("第二连接被拒绝")) ||
               message.contains(QStringLiteral("single_device_only"))) {
        SetBehavior(QStringLiteral("第二设备已被拒绝，保持一对一会话"),
                    QStringLiteral("host_ack: busy"));
    } else if (message.contains(QStringLiteral("会话已恢复"))) {
        Activate(5, QStringLiteral("恢复会话"));
        SetBehavior(QStringLiteral("TCP 已重连，正在恢复原会话"), QStringLiteral("device_hello + session_id"));
    }
    update();
}

void HandshakeFlowWidget::Reset(const QString &behavior)
{
    for (Node &node : nodes_)
        node.state = NodeState::Pending;
    nodes_[0].detail = QStringLiteral("初始化");
    nodes_[1].detail = QStringLiteral("移动热点");
    nodes_[2].detail = QStringLiteral("192.168.137.1/24");
    nodes_[3].detail = QStringLiteral("0.0.0.0:5935");
    nodes_[4].detail = QStringLiteral("device_hello");
    nodes_[5].detail = QStringLiteral("ping / pong");
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
    return QSize(760, 180);
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
                     QStringLiteral("端到端握手与连接状态 · 上位机视角"));

    QFont small_font = painter.font();
    small_font.setBold(false);
    small_font.setPointSize(8);
    painter.setFont(small_font);
    const QString legend = QStringLiteral("● 进行中    ✓ 已完成    × 失败    ↻ 重试 / 自愈");
    painter.setPen(QColor("#64748b"));
    painter.drawText(QRectF(18, 33, width() - 36, 18), legend);

    const int columns = kNodeCount;
    const qreal gap = 14.0;
    const qreal left = 18.0;
    const qreal node_width = (width() - left * 2 - gap * (columns - 1)) / columns;
    const qreal node_height = 61.0;
    const qreal first_y = 56.0;
    QVector<QRectF> boxes;
    boxes.reserve(nodes_.size());
    for (int index = 0; index < nodes_.size(); ++index) {
        boxes.append(QRectF(left + index * (node_width + gap), first_y,
                            node_width, node_height));
    }

    painter.setPen(QPen(QColor("#94a3b8"), 2));
    for (int index = 0; index + 1 < boxes.size(); ++index) {
        const QPointF start = QPointF(boxes[index].right(), boxes[index].center().y());
        const QPointF end = QPointF(boxes[index + 1].left(), boxes[index + 1].center().y());
        painter.drawLine(start, end);
        QPainterPath arrow;
        arrow.moveTo(end);
        arrow.lineTo(end + QPointF(-7, -4));
        arrow.lineTo(end + QPointF(-7, 4));
        arrow.closeSubpath();
        painter.fillPath(arrow, QColor("#94a3b8"));
    }

    for (int index = 0; index < nodes_.size(); ++index) {
        const Node &node = nodes_[index];
        const QRectF box = boxes[index];
        const QColor color = StateColor(node.state);
        painter.setPen(QPen(color, node.state == NodeState::Active ? 2.5 : 1.5));
        painter.setBrush(node.state == NodeState::Pending ? QColor("#ffffff")
                                                          : QColor(color.red(), color.green(), color.blue(), 18));
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
