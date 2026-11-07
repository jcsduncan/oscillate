#pragma once

#include "oscillate/application/nova_arp_patterns.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <string_view>
#include <vector>

namespace oscillate::quick {

class NativeSynthDeviceController;

// Nova's arp pattern engine panel model (ADR-0078, the #arp tab of
// docs/ui/creative-tools-mockup.html): the pattern library, MUTATE with
// per-lane locks and a seed, and the arp's groove from the groove pool.
// Every change is one edit (one undo step) of the arp's parameters and its
// document labels (arp.pattern, arp.groove); nothing outside nova.arp.*
// changes.
class NovaArp final : public QObject {
  Q_OBJECT
  // The factory patterns, [{index, name, category, mode, rate}], in the
  // library's order.
  Q_PROPERTY(QVariantList patterns READ patterns CONSTANT)
  Q_PROPERTY(QStringList categories READ categories CONSTANT)
  // The modes, [{index, name, group}], grouped as the panel shows them.
  Q_PROPERTY(QVariantList modes READ modes CONSTANT)
  // The lanes, [{id, name, kind ("level", "integer", "toggle", "choice"),
  // minimum, maximum, fallback, choices}], in parameter order; and the most
  // steps a lane holds.
  Q_PROPERTY(QVariantList lanes READ lanes CONSTANT)
  Q_PROPERTY(int maxSteps READ max_steps CONSTANT)
  // Lanes a pattern load or MUTATE leaves alone (lane ids).
  Q_PROPERTY(QStringList locks READ locks NOTIFY locks_changed)
  // MUTATE's seed now (0: the pattern as it was before the first mutation)
  // and how much it changes, 0..1.
  Q_PROPERTY(int mutation READ mutation NOTIFY mutation_changed)
  Q_PROPERTY(double amount READ amount WRITE set_amount NOTIFY mutation_changed)
  // Why the last request did nothing, or "".
  Q_PROPERTY(QString message READ message NOTIFY message_changed)

public:
  static constexpr std::string_view pattern_label = native_dsp::synth::nova::arp_pattern_label;
  static constexpr std::string_view groove_label = native_dsp::synth::nova::arp_groove_label;

  explicit NovaArp(NativeSynthDeviceController& controller, QObject* parent = nullptr);

  [[nodiscard]] QVariantList patterns() const;
  [[nodiscard]] QStringList categories() const;
  [[nodiscard]] QVariantList modes() const;
  [[nodiscard]] QVariantList lanes() const;
  [[nodiscard]] int max_steps() const noexcept;
  [[nodiscard]] QStringList locks() const;
  [[nodiscard]] int mutation() const noexcept { return static_cast<int>(mutation_); }
  [[nodiscard]] double amount() const noexcept { return amount_; }
  void set_amount(double amount);
  [[nodiscard]] QString message() const { return message_; }

  // Loads factory pattern `index` (into the unlocked lanes); one edit.
  Q_INVOKABLE bool apply_pattern(int index);
  // The pattern's index by name, or -1.
  Q_INVOKABLE int find_pattern(const QString& name) const;
  // MUTATE: the sound before the first mutation (or before the last edit
  // since one) varied by `seed` at `amount`; seed 0 returns to it. One
  // edit each.
  Q_INVOKABLE bool mutate(int seed);
  Q_INVOKABLE bool set_lock(const QString& lane, bool locked);
  Q_INVOKABLE bool is_locked(const QString& lane) const;
  // A groove-pool entry ({name, stepsPerQuarter, timings, velocities}, as
  // NoteToolsController.grooves lists them) as the arp's groove; or none.
  Q_INVOKABLE bool set_groove(const QVariantMap& groove);
  Q_INVOKABLE bool clear_groove();
  // Steps before the whole pattern repeats (the lanes' lengths' least
  // common multiple, and the Euclidean cycle in that mode).
  Q_INVOKABLE double period() const;
  // The same in quarter-note beats at the arp's rate.
  Q_INVOKABLE double period_beats() const;
  // The Euclidean rhythm now, one bool (a hit) per step of its cycle.
  Q_INVOKABLE QVariantList euclid_steps() const;

signals:
  void locks_changed();
  void mutation_changed();
  void message_changed();

private:
  bool commit(std::vector<double> values, const QString* pattern, const QString* groove);
  void set_message(const QString& message);

  NativeSynthDeviceController& controller_;
  application::nova_arp::LaneLocks locks_{};
  std::vector<double> base_;
  std::uint32_t mutation_ = 0;
  double amount_ = application::nova_arp::default_mutation_amount;
  // The controller's revision after this model's last edit: anything else
  // since then is the user's, and MUTATE starts from it.
  int written_revision_ = -1;
  QString message_;
};

} // namespace oscillate::quick
