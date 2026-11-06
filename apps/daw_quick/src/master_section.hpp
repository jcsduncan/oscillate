#pragma once

#include "true_peak_monitor.hpp"

#include <QObject>
#include <QVariantMap>

#include <atomic>
#include <memory>

namespace oscillate::quick {

// The master section's readings for QML: loudness (momentary, short-term,
// integrated LUFS), true peak and the mono check (correlation and how much
// quieter the mix gets in mono), all measured from the final output by the
// master analysis worker (LiveTruePeakMonitor). The momentary Mono switch
// folds what the device hears; the meters and export never change.
class MasterSection final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap reading READ reading NOTIFY changed)
  Q_PROPERTY(bool monoListen READ mono_listen WRITE set_mono_listen NOTIFY monoListenChanged)

public:
  MasterSection(std::shared_ptr<LiveTruePeakMonitor> analysis,
                std::shared_ptr<std::atomic<bool>> mono_listen, QObject* parent = nullptr);

  // {valid, momentary, shortTerm, integrated, truePeakDb, correlation,
  // monoLossDb}: a missing number is NaN, which QML shows as a dash.
  [[nodiscard]] QVariantMap reading() const { return reading_; }
  [[nodiscard]] bool mono_listen() const noexcept;
  void set_mono_listen(bool on);
  // Integrated loudness starts again from now; the true peak holds.
  Q_INVOKABLE void reset_loudness();
  // Reads the newest analysis; the bridge calls it at the meter cadence.
  Q_INVOKABLE void poll();

signals:
  void changed();
  void monoListenChanged();

private:
  std::shared_ptr<LiveTruePeakMonitor> analysis_;
  std::shared_ptr<std::atomic<bool>> mono_listen_;
  QVariantMap reading_;
};

} // namespace oscillate::quick
