#include "keyed_list_model.hpp"

#include <QSet>

#include <utility>

namespace oscillate::quick {

KeyedListModel::KeyedListModel(QStringList extra_roles, QObject* parent)
    : QAbstractListModel(parent), extra_roles_(std::move(extra_roles)) {}

int KeyedListModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : count();
}

QVariant KeyedListModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= count())
    return {};
  const auto& row = rows_[static_cast<std::size_t>(index.row())];
  if (role == data_role)
    return row.data;
  if (role == id_role)
    return row.id;
  const auto extra = static_cast<std::size_t>(role - first_extra_role);
  return role >= first_extra_role && extra < row.extras.size() ? row.extras[extra] : QVariant{};
}

QHash<int, QByteArray> KeyedListModel::roleNames() const {
  QHash<int, QByteArray> names{{data_role, "modelData"}, {id_role, "rowId"}};
  for (int index = 0; index < extra_roles_.size(); ++index)
    names.insert(first_extra_role + index, extra_roles_[index].toUtf8());
  return names;
}

int KeyedListModel::find(const QString& id, int from) const {
  for (int index = from; index < count(); ++index)
    if (rows_[static_cast<std::size_t>(index)].id == id)
      return index;
  return -1;
}

void KeyedListModel::assign(int index, Row row) {
  auto& current = rows_[static_cast<std::size_t>(index)];
  QList<int> changed;
  if (current.data != row.data) {
    current.data = std::move(row.data);
    changed.push_back(data_role);
  }
  for (std::size_t extra = 0; extra < row.extras.size(); ++extra) {
    if (current.extras.size() <= extra)
      current.extras.resize(extra + 1);
    if (current.extras[extra] != row.extras[extra]) {
      current.extras[extra] = std::move(row.extras[extra]);
      changed.push_back(first_extra_role + static_cast<int>(extra));
    }
  }
  if (!changed.isEmpty())
    emit dataChanged(this->index(index), this->index(index), changed);
}

void KeyedListModel::update(std::vector<Row> rows) {
  const int before = count();
  QSet<QString> wanted;
  for (const auto& row : rows)
    wanted.insert(row.id);
  for (int index = count() - 1; index >= 0; --index) {
    if (wanted.contains(rows_[static_cast<std::size_t>(index)].id))
      continue;
    beginRemoveRows({}, index, index);
    rows_.erase(rows_.begin() + index);
    endRemoveRows();
  }
  for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
    auto& row = rows[static_cast<std::size_t>(index)];
    const int found = find(row.id, index);
    if (found < 0) {
      beginInsertRows({}, index, index);
      rows_.insert(rows_.begin() + index, std::move(row));
      endInsertRows();
      continue;
    }
    if (found != index) {
      beginMoveRows({}, found, found, {}, index);
      auto moved = std::move(rows_[static_cast<std::size_t>(found)]);
      rows_.erase(rows_.begin() + found);
      rows_.insert(rows_.begin() + index, std::move(moved));
      endMoveRows();
    }
    assign(index, std::move(row));
  }
  if (count() != before)
    emit countChanged();
}

void KeyedListModel::set_extra(const QString& id, int extra, const QVariant& value) {
  const int index = find(id);
  if (index < 0 || extra < 0 || extra >= extra_roles_.size())
    return;
  std::vector<QVariant> extras(static_cast<std::size_t>(extra) + 1);
  auto& row = rows_[static_cast<std::size_t>(index)];
  for (std::size_t item = 0; item < extras.size() - 1; ++item)
    extras[item] = item < row.extras.size() ? row.extras[item] : QVariant{};
  extras.back() = value;
  assign(index, Row{id, row.data, std::move(extras)});
}

QVariant KeyedListModel::extra(const QString& id, int extra) const {
  const int index = find(id);
  if (index < 0 || extra < 0)
    return {};
  const auto& extras = rows_[static_cast<std::size_t>(index)].extras;
  return static_cast<std::size_t>(extra) < extras.size() ? extras[static_cast<std::size_t>(extra)]
                                                         : QVariant{};
}

QVariantMap KeyedListModel::get(int index) const {
  return index >= 0 && index < count() ? rows_[static_cast<std::size_t>(index)].data
                                       : QVariantMap{};
}

int KeyedListModel::indexOf(const QString& id) const { return find(id); }

} // namespace oscillate::quick
