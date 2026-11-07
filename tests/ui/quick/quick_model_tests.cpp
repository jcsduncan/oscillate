#include "../../../apps/daw_quick/src/keyed_list_model.hpp"
#include "../../../apps/daw_quick/src/meter_model.hpp"
#include "oscillate/test/check.hpp"

#include <QCoreApplication>
#include <QSignalSpy>

#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {

using namespace oscillate;

using oscillate::test::require;

double db(double amplitude) { return 20.0 * std::log10(amplitude); }

void peaks_rise_at_once_and_fall_steadily() {
  quick::MeterChannel channel;
  const quick::MeterReading loud{.left_peak = 1.0F,
                                 .right_peak = 0.5F,
                                 .left_rms = 0.5F,
                                 .right_rms = 0.25F,
                                 .clipped = false};
  require(channel.advance(loud, 0.016) && channel.valid() && channel.left_peak() == 1.0 &&
              channel.right_peak() == 0.5,
          "a peak did not show at once");
  // Silence: the peak falls at its rate, the hold waits, then falls too.
  channel.advance(quick::MeterReading{}, 0.25);
  channel.advance(quick::MeterReading{}, 0.25);
  require(std::abs(db(channel.left_peak()) + 13.0) < 1e-6 && channel.left_hold() == 1.0,
          "the peak did not fall at 26 dB/s, or the hold did not wait");
  for (int step = 0; step < 4; ++step)
    channel.advance(quick::MeterReading{}, 0.25);
  require(channel.left_hold() < 1.0 && channel.left_hold() >= channel.left_peak(),
          "the hold did not fall after waiting, or fell below the peak");
  // A louder window replaces a falling peak at once, without a glide.
  channel.advance(quick::MeterReading{.left_peak = 0.8F}, 0.016);
  require(channel.left_peak() == static_cast<double>(0.8F), "a new peak glided up");
}

void rms_is_smoothed_and_clips_stay_lit() {
  quick::MeterChannel channel;
  const quick::MeterReading steady{
      .left_peak = 0.5F, .right_peak = 0.5F, .left_rms = 0.5F, .right_rms = 0.5F, .clipped = true};
  channel.advance(steady, 0.016);
  require(channel.left_rms() > 0.0 && channel.left_rms() < 0.2 && channel.clipped(),
          "RMS jumped instead of integrating, or a clip did not light");
  for (int step = 0; step < 200; ++step)
    channel.advance(
        quick::MeterReading{
            .left_peak = 0.5F, .right_peak = 0.5F, .left_rms = 0.5F, .right_rms = 0.5F},
        0.016);
  require(std::abs(channel.left_rms() - 0.5) < 1e-3 && !channel.clipped(),
          "RMS did not settle on a steady level, or the clip light never went out");
}

void stopped_meters_fall_to_silence_and_go_idle() {
  quick::MeterChannel channel;
  channel.advance(quick::MeterReading{.left_peak = 1.0F, .left_rms = 1.0F}, 0.016);
  // No reading (stopped): still visible while falling, then invalid.
  require(channel.advance(std::nullopt, 0.1) && channel.valid(),
          "a stopped meter vanished instead of falling");
  bool visible = true;
  for (int step = 0; step < 100 && visible; ++step)
    visible = channel.advance(std::nullopt, 0.25);
  require(!visible && !channel.valid() && channel.left_peak() == 0.0 && channel.left_rms() == 0.0,
          "a stopped meter never fell to silence");
  // Invalid engine values never reach the display.
  channel.advance(quick::MeterReading{.left_peak = std::nanf(""), .left_rms = -1.0F}, 0.016);
  require(channel.left_peak() == 0.0 && channel.left_rms() == 0.0, "a non-finite level showed");
}

void the_model_keeps_one_channel_per_id() {
  int reads = 0;
  quick::MeterModel model([&reads](const QString& id) -> std::optional<quick::MeterReading> {
    ++reads;
    if (id == QStringLiteral("a"))
      return quick::MeterReading{.left_peak = 0.5F};
    return std::nullopt;
  });
  auto* first = qobject_cast<quick::MeterChannel*>(model.channel(QStringLiteral("a")));
  auto* master = qobject_cast<quick::MeterChannel*>(model.channel(QString{}));
  require(first && master && model.channel(QStringLiteral("a")) == first && first != master,
          "channels were not kept per ID");
  int notified = 0;
  QObject::connect(master, &quick::MeterChannel::changed, [&notified] { ++notified; });
  model.tick(0.016);
  require(reads == 2 && first->left_peak() == static_cast<double>(0.5F) && !master->valid() &&
              notified == 1,
          "a tick did not read every channel once");
}

using Row = quick::KeyedListModel::Row;

QVariantMap fields(const QString& name) { return {{"name", name}}; }

void keyed_rows_signal_only_what_changed() {
  quick::KeyedListModel model({QStringLiteral("waveform")});
  model.update({{"a", fields("A")}, {"b", fields("B")}, {"c", fields("C")}});
  require(model.count() == 3 && model.get(1).value("name") == "B" && model.indexOf("c") == 2,
          "rows were not set in order");
  QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
  QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
  QSignalSpy moved(&model, &QAbstractItemModel::rowsMoved);
  QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
  QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
  // The same rows again: no signal at all.
  model.update({{"a", fields("A")}, {"b", fields("B")}, {"c", fields("C")}});
  require(inserted.isEmpty() && removed.isEmpty() && moved.isEmpty() && changed.isEmpty(),
          "an unchanged update signalled");
  // One row's fields change: one dataChanged, for that row and role only.
  model.update({{"a", fields("A")}, {"b", fields("B2")}, {"c", fields("C")}});
  require(changed.size() == 1 && changed[0][0].toModelIndex().row() == 1 &&
              changed[0][2].value<QList<int>>().size() == 1,
          "a changed row did not signal alone");
  changed.clear();
  // Remove one, insert one, move one: row signals, never a reset.
  model.update({{"c", fields("C")}, {"d", fields("D")}, {"a", fields("A")}});
  require(removed.size() == 1 && inserted.size() == 1 && moved.size() == 1 && reset.isEmpty() &&
              changed.isEmpty() && model.count() == 3 && model.indexOf("c") == 0 &&
              model.indexOf("d") == 1 && model.indexOf("a") == 2 &&
              model.get(1).value("name") == "D",
          "rows were not inserted, removed and moved by ID");
  // An extra role changes without touching the row's fields.
  model.set_extra("a", 0, QVariantList{1, 2});
  model.set_extra("a", 0, QVariantList{1, 2});
  require(changed.size() == 1 && changed[0][0].toModelIndex().row() == 2 &&
              model.extra("a", 0).toList().size() == 2 &&
              model.data(model.index(2), Qt::UserRole + 1).toMap().value("name") == "A",
          "an extra role did not signal once, or touched the fields");
  // Updating fields keeps an extra the update does not name.
  model.update({{"c", fields("C")}, {"d", fields("D")}, {"a", fields("A2")}});
  require(model.extra("a", 0).toList().size() == 2, "a field update dropped an extra role");
  const auto roles = model.roleNames();
  require(roles.value(Qt::UserRole + 1) == "modelData" &&
              roles.value(Qt::UserRole + 2) == "rowId" &&
              roles.value(Qt::UserRole + 3) == "waveform",
          "role names are not modelData, rowId and the extras");
}

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    peaks_rise_at_once_and_fall_steadily();
    rms_is_smoothed_and_clips_stay_lit();
    stopped_meters_fall_to_silence_and_go_idle();
    the_model_keeps_one_channel_per_id();
    keyed_rows_signal_only_what_changed();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
