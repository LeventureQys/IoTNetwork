#include "flow_widget.h"

#include <QHBoxLayout>
#include <QLabel>

namespace {
const char *kStepNames[] = {
    "启动", "扫描PC热点", "连接热点", "连接PC", "会话在线", "异常重连"
};
}

FlowWidget::FlowWidget(QWidget *parent)
    : QWidget(parent)
{
    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    for (const char *name : kStepNames) {
        QLabel *label = new QLabel(QString::fromUtf8(name), this);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet("QLabel { border:1px solid #888; border-radius:3px; padding:2px 6px; color:#888; }");
        step_labels_.append(label);
        layout->addWidget(label);
    }
    online_label_ = new QLabel(tr("未连接"), this);
    online_label_->setStyleSheet("QLabel { color:#888; font-weight:bold; }");
    layout->addWidget(online_label_);
    setMinimumHeight(36);
}

void FlowWidget::SetDeviceState(int state)
{
    if (current_state_ == state)
        return;
    current_state_ = state;
    RefreshStyle();
}

void FlowWidget::SetOnline(bool online)
{
    if (online_ == online)
        return;
    online_ = online;
    if (online_label_) {
        online_label_->setText(online ? tr("会话在线") : tr("未连接"));
        online_label_->setStyleSheet(online
                                         ? "QLabel { color:#2e7d32; font-weight:bold; }"
                                         : "QLabel { color:#888; font-weight:bold; }");
    }
}

void FlowWidget::RefreshStyle()
{
    for (int i = 0; i < step_labels_.size(); ++i) {
        bool active = (i == current_state_);
        step_labels_[i]->setStyleSheet(
            active
                ? QStringLiteral("QLabel { border:1px solid #2e7d32; border-radius:3px; "
                                 "padding:2px 6px; color:#2e7d32; font-weight:bold; }")
                : QStringLiteral("QLabel { border:1px solid #888; border-radius:3px; "
                                 "padding:2px 6px; color:#888; }"));
    }
}
