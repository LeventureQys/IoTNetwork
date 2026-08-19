#ifndef DEMO_DEVICE_UI_FLOW_WIDGET_H
#define DEMO_DEVICE_UI_FLOW_WIDGET_H

#include <QList>
#include <QWidget>

class QLabel;

/* 配网/会话流程指示（6 个设备状态步骤，高亮当前状态）。纯展示控件。 */
class FlowWidget : public QWidget {
    Q_OBJECT
public:
    explicit FlowWidget(QWidget *parent = nullptr);

    void SetDeviceState(int state);
    void SetOnline(bool online);

private:
    void RefreshStyle();

    QList<QLabel *> step_labels_;
    QLabel *online_label_;
    int current_state_ = -1;
    bool online_ = false;
};

#endif
