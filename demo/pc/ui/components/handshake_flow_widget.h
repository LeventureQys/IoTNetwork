#ifndef DEMO_HANDSHAKE_FLOW_WIDGET_H
#define DEMO_HANDSHAKE_FLOW_WIDGET_H

#include <QMutex>
#include <QStringList>
#include <QVector>
#include <QWidget>

class HandshakeFlowWidget : public QWidget {
public:
    enum class Side { Host, Device };
    enum class NodeState { Pending, Active, Success, Failed, Retrying };

    explicit HandshakeFlowWidget(Side side, QWidget *parent = nullptr);

    void PushLog(const QString &message);
    void DrainEvents();
    void SetDeviceState(int state);
    void SetHostState(bool provisioning, int done, int total, int registered, int online);

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

    Side side_;
    QVector<Node> nodes_;
    QString behavior_;
    QString protocol_;
    QStringList pending_logs_;
    QMutex pending_mutex_;
    bool heartbeat_seen_ = false;
};

#endif
