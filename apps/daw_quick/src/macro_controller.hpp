#pragma once

#include "macro_map_state.hpp"

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/project/model.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::application {
class ProjectSession;
}

namespace oscillate::quick {

// The macro strip at the head of a track's device chain (ADR-0059): eight
// knobs, each driving the parameters mapped to it. A turn is heard as it
// moves through the running graph's live value mailboxes, with no saved
// revision per step, and the panels showing those devices follow it; the
// release saves the macro and every mapped parameter as one undo step.
class MacroController final : public QObject {
  Q_OBJECT
  // The track whose macros the strip shows (a track id; empty for none).
  Q_PROPERTY(QString track_id READ track_id WRITE set_track_id NOTIFY changed)
  Q_PROPERTY(bool available READ available NOTIFY changed)
  // Eight entries {index, name, label, value, value_text, mappings
  // [{label, device, parameter, minimum, maximum (knob positions 0..1),
  // minimum_text, maximum_text}]}.
  Q_PROPERTY(QVariantList macros READ macros NOTIFY changed)
  // The macro being turned, or -1.
  Q_PROPERTY(int turning READ turning NOTIFY changed)
  // The macro MAP mode is mapping, or -1 (shared with every device panel).
  Q_PROPERTY(int armed READ armed WRITE set_armed NOTIFY armed_changed)

public:
  struct Hooks {
    std::function<application::ProjectSession*()> session;
    std::function<bool()> can_edit;
    std::function<void(QString)> report;
    // After an accepted edit: offer it to playback and refresh the views.
    std::function<void()> after_edit;
    std::function<void()> save;
    // Live values for the running synth or effect, in table order; false
    // when nothing plays it.
    std::function<bool(project::TrackId, const native_dsp::synth::EngineDescriptor&,
                       std::span<const double>)>
        live_synth;
    std::function<bool(project::TrackId, project::InsertSlotId,
                       const native_dsp::synth::EngineDescriptor&, std::span<const double>)>
        live_effect;
    // Panels showing the synth (no slot) or effect follow these unsaved
    // values; null hands them back to the project.
    std::function<void(project::TrackId, std::optional<project::InsertSlotId>,
                       const std::vector<double>*)>
        show;
  };

  explicit MacroController(Hooks hooks, QObject* parent = nullptr);

  [[nodiscard]] QString track_id() const { return track_id_; }
  void set_track_id(const QString& id);
  [[nodiscard]] bool available() const;
  [[nodiscard]] QVariantList macros() const { return macros_; }
  [[nodiscard]] int turning() const noexcept { return turn_ ? turn_->index : -1; }
  [[nodiscard]] int armed() const noexcept { return map_state_.armed(); }
  void set_armed(int index) { map_state_.set_armed(index); }
  [[nodiscard]] MacroMapState& map_state() noexcept { return map_state_; }

  // A drag: begin, any number of turns (heard, not saved), then end (one
  // saved edit) or cancel (the saved sound back).
  Q_INVOKABLE void begin_turn(int index);
  Q_INVOKABLE void turn(int index, double value);
  Q_INVOKABLE void end_turn(int index);
  Q_INVOKABLE void cancel_turn(int index);
  // One complete edit: a click, the wheel, a key or a reset.
  Q_INVOKABLE void set_value(int index, double value);
  Q_INVOKABLE void step(int index, int direction, bool fine);
  Q_INVOKABLE void rename(int index, const QString& name);
  // A mapping's range as knob positions (0..1 of the parameter's travel).
  Q_INVOKABLE void set_mapping_range(int index, int mapping, double minimum, double maximum);
  Q_INVOKABLE void remove_mapping(int index, int mapping);
  // The parameter's value text at a knob position, for range-handle drags.
  Q_INVOKABLE QString position_text(int index, int mapping, double position) const;
  Q_INVOKABLE void refresh();

signals:
  void changed();
  void armed_changed();

private:
  // One device a turning macro drives: its saved dense values and, per
  // mapped parameter, the table index and the mapping.
  struct TurnDevice {
    std::optional<project::InsertSlotId> slot;
    const native_dsp::synth::EngineDescriptor* descriptor = nullptr;
    std::vector<double> base;
    std::vector<std::pair<std::size_t, project::MacroMapping>> mapped;
  };
  struct Turn {
    int index = -1;
    double value = 0.0;
    std::vector<TurnDevice> devices;
    // The session the turn began in. Its saved values and devices belong to
    // that session: a turn outliving it offers, commits and restores nothing.
    std::uint64_t session = 0;
  };
  [[nodiscard]] const project::Track* track() const;
  [[nodiscard]] std::optional<project::TrackId> track_key() const;
  void rebuild();
  void offer(const Turn& turn, double value);
  // The panels follow the project again.
  void hand_back(const Turn& turn);
  // The saved values go back to the running devices.
  void restore(const Turn& turn);
  // The turn began in a session that has since been replaced.
  [[nodiscard]] bool turn_outlived_session() const;
  // Ends a turn that outlived its session, without touching any device.
  void drop_turn();
  bool commit_value(int index, double value);

  void accepted();

  Hooks hooks_;
  MacroMapState map_state_;
  QString track_id_;
  QVariantList macros_;
  std::optional<Turn> turn_;
};

} // namespace oscillate::quick
