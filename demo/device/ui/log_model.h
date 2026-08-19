#ifndef DEMO_DEVICE_UI_LOG_MODEL_H
#define DEMO_DEVICE_UI_LOG_MODEL_H

#include <QAbstractListModel>
#include <QList>
#include <QString>

/* 设备日志模型：DeviceWindow 在 GUI 线程从 C 队列 drain 后调用 Append 追加。
 * 不注册 Qt 对象为 C 全局 sink（facade pull 模式）。 */
class LogModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum { MaxLines = 2000 };

    explicit LogModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    void Append(const QString &line);

private:
    QList<QString> lines_;
};

#endif
