#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <vector>

namespace oscillate::quick {

// A QML list model of rows keyed by a stable ID, updated by diffing: each
// update inserts, removes and moves rows by ID and signals only the roles
// whose values changed, so a delegate is never rebuilt for an edit to
// another row, and rebinds only when its own row changes.
//
// Every row exposes its fields as `modelData` (a map, so delegates read
// `modelData.name` as with a JS array) and `rowId`, plus any extra roles
// named at construction. Extras change on their own (set_extra), without
// touching modelData: a clip's waveform, a track's clip model.
class KeyedListModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
  struct Row {
    QString id;
    QVariantMap data;
    // In the order of the extra role names; missing ones are left as they are.
    std::vector<QVariant> extras = {};
  };

  explicit KeyedListModel(QStringList extra_roles = {}, QObject* parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  // Makes the rows exactly `rows`, in order, with the fewest row signals.
  void update(std::vector<Row> rows);
  // Sets one extra role of one row; signals only if the value changed.
  void set_extra(const QString& id, int extra, const QVariant& value);
  [[nodiscard]] QVariant extra(const QString& id, int extra) const;

  [[nodiscard]] int count() const noexcept { return static_cast<int>(rows_.size()); }
  // The row's fields, or an empty map.
  Q_INVOKABLE QVariantMap get(int index) const;
  Q_INVOKABLE int indexOf(const QString& id) const;
  [[nodiscard]] const std::vector<Row>& rows() const noexcept { return rows_; }

signals:
  void countChanged();

private:
  static constexpr int data_role = Qt::UserRole + 1;
  static constexpr int id_role = Qt::UserRole + 2;
  static constexpr int first_extra_role = Qt::UserRole + 3;

  [[nodiscard]] int find(const QString& id, int from = 0) const;
  void assign(int index, Row row);

  QStringList extra_roles_;
  std::vector<Row> rows_;
};

} // namespace oscillate::quick
