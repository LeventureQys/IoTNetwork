#ifndef DEMO_HANDSHAKE_FLOW_WIDGET_H
#define DEMO_HANDSHAKE_FLOW_WIDGET_H

#include <QMutex>
#include <QStringList>
#include <QVector>
#include <QWidget>

/* PC 端一对一流程节点：启动、热点启动、固定 IP、TCP 监听、设备握手、会话在线。 */
class HandshakeFlowWidget : public QWidget {
public:
    enum class NodeState { Pending, Active, Success, Failed, Retrying };

    explicit HandshakeFlowWidget(QWidget *parent = nullptr);

    void PushLog(const QString &message);
    void DrainEvents();
    /* registered：已注册设备数；online：在线设备数 */
    void SetHostState(int registered, int online);

    /* 测试观察口：节点数量与标题（不暴露内部状态） */
    int nodeCount() const { return nodes_.size(); }
    QStringList nodeTitles() const
    {
        QStringList titles;
        for (const Node &node : nodes_)
            titles.append(node.title);
        return titles;
    }

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize minimumSizeHint() const override;

private:
    struct Node {
        QString title;
        QString detail;
        NodeState state = NodeState::Pending;
    };

    void ApplyLog(const QString &message);
    void Reset(const QString &behavior);
    void Activate(int index, const QString &detail = QString());
    void Complete(int index, const QString &detail = QString());
    void Fail(int index, const QString &detail);
    void Retry(int index, const QString &detail);
    void MarkThrough(int index);
    void SetBehavior(const QString &behavior, const QString &protocol = QString());

    QVector<Node> nodes_;
    QString behavior_;
    QString protocol_;
    QStringList pending_logs_;
    QMutex pending_mutex_;
    bool heartbeat_seen_ = false;
};

#endif
