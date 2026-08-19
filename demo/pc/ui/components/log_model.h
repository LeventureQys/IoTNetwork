#ifndef DEMO_LOG_MODEL_H
#define DEMO_LOG_MODEL_H

#include <QAbstractListModel>
#include <QMutex>
#include <QStringList>
#include <deque>

/* 日志模型：log sink 回调（任意线程）入队，UI 定时器在 GUI 线程批量搬运 */
class LogModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum { MaxLines = 2000 };

    explicit LogModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    /* 任意线程调用（log sink 回调） */
    void PushLog(int level, const QString &line);
    /* GUI 线程调用（QTimer）：搬运队列到模型 */
    void Drain();

private:
    QMutex pending_mu_;
    std::deque<QString> lines_;
    std::deque<QString> pending_;
};

#endif
