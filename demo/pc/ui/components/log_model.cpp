#include "log_model.h"

LogModel::LogModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int LogModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return (int)lines_.size();
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();
    if (index.row() < 0 || index.row() >= (int)lines_.size())
        return QVariant();
    if (role == Qt::DisplayRole)
        return lines_[index.row()];
    return QVariant();
}

void LogModel::PushLog(int level, const QString &line)
{
    Q_UNUSED(level);
    QMutexLocker locker(&pending_mu_);
    if ((int)pending_.size() >= MaxLines)
        pending_.pop_front();
    pending_.push_back(line);
}

void LogModel::Drain()
{
    std::deque<QString> batch;
    {
        QMutexLocker locker(&pending_mu_);
        batch.swap(pending_);
    }
    if (batch.empty())
        return;

    const int remove_count = qMax(0, (int)(lines_.size() + batch.size()) - MaxLines);
    if (remove_count > 0) {
        beginRemoveRows(QModelIndex(), 0, remove_count - 1);
        for (int i = 0; i < remove_count; ++i)
            lines_.pop_front();
        endRemoveRows();
    }

    const int first = (int)lines_.size();
    const int added = (int)batch.size();
    beginInsertRows(QModelIndex(), first, first + added - 1);
    while (!batch.empty()) {
        lines_.push_back(std::move(batch.front()));
        batch.pop_front();
    }
    endInsertRows();
}
