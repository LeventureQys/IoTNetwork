#include "log_model.h"

LogModel::LogModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int LogModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : lines_.size();
}

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= lines_.size())
        return QVariant();
    if (role == Qt::DisplayRole)
        return lines_.at(index.row());
    return QVariant();
}

void LogModel::Append(const QString &line)
{
    beginInsertRows(QModelIndex(), lines_.size(), lines_.size());
    lines_.append(line);
    endInsertRows();
    while (lines_.size() > MaxLines) {
        beginRemoveRows(QModelIndex(), 0, 0);
        lines_.removeFirst();
        endRemoveRows();
    }
}
