#pragma once

#include "native_synth_device_controller.hpp"
#include "native_synth_panel_adapter.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/project/model.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oscillate::quick {

// NativeSynthPanelAdapter over one note effect slot of a track (ADR-0059),
// so the device kit's knobs, presets and macro mapping work on MIDI effects
// as on synths and effects. A drag saves each move under one gesture (one
// undo step); the running player takes each saved revision live.
class NoteEffectAdapter final : public NativeSynthPanelAdapter {
public:
  using Hook = std::function<void()>;

  // `engine` must outlive the adapter (registered engines are static).
  // after_edit runs after a finished edit (refresh, offer to playback),
  // live_edit after a drag's move (offer to playback), save after a
  // finished edit.
  NoteEffectAdapter(application::ProjectSession& session, project::TrackId track,
                    project::InsertSlotId slot,
                    const native_dsp::note_effects::NoteEffectEngine& engine, Hook after_edit,
                    Hook live_edit, Hook save);

  [[nodiscard]] std::vector<double> values() const override;
  void submit(std::span<const double> values, NativeSynthGesture gesture) override;
  [[nodiscard]] NativeSynthPanelStatus status() const override;
  bool submit_preset(std::size_t preset) override;
  [[nodiscard]] std::optional<std::uint32_t> macro_of(std::string_view parameter_id) const override;
  bool map_to_macro(std::string_view parameter_id, std::uint32_t index, double minimum,
                    double maximum) override;
  bool unmap_macro(std::string_view parameter_id) override;
  [[nodiscard]] std::vector<std::pair<std::string, std::uint32_t>> macro_bindings() const override;

private:
  [[nodiscard]] const project::NoteEffectInsert* insert() const;
  [[nodiscard]] bool write(std::span<const double> values, std::optional<std::uint64_t> gesture);
  void finished();

  application::ProjectSession& session_;
  project::TrackId track_;
  project::InsertSlotId slot_;
  const native_dsp::note_effects::NoteEffectEngine& engine_;
  Hook after_edit_;
  Hook live_edit_;
  Hook save_;
  // Panel gestures use their own range so they never merge with other edits.
  std::uint64_t next_gesture_ = 0x4e46'0000'0000'0001ULL;
  std::optional<std::uint64_t> gesture_;
  std::vector<double> before_gesture_;
  std::string message_;
};

// The MIDI effects of a track's built-in synth (ADR-0059) for QML: the
// engines for the browser, each track's chain, chain edits (each one undo
// step) and a device-kit controller per slot.
class NoteEffectsController final : public QObject {
  Q_OBJECT
  // [{id, name, presets: [{index, name, category}]}], in registry order.
  Q_PROPERTY(QVariantList engines READ engines CONSTANT)

public:
  using Session = std::function<application::ProjectSession*()>;
  using CanEdit = std::function<bool()>;
  using Report = std::function<void(QString)>;
  using Hook = std::function<void()>;
  // Runs once for each device controller made (the shell shares its macro
  // MAP mode with every panel).
  using ControllerCreated = std::function<void(NativeSynthDeviceController&)>;

  NoteEffectsController(Session session, CanEdit can_edit, Report report, Hook after_edit,
                        Hook live_edit, Hook save, ControllerCreated created = {},
                        QObject* parent = nullptr);
  ~NoteEffectsController() override;

  [[nodiscard]] QVariantList engines() const;
  // True when the track plays a built-in synth, which MIDI effects feed.
  Q_INVOKABLE bool accepts(const QString& track_id) const;
  // [{slotId, engineId, name, enabled}], first to last.
  Q_INVOKABLE QVariantList chain(const QString& track_id) const;
  // Adds `engine` at `index` (-1: last) from factory preset `preset`
  // (-1: defaults); returns the new slot's id, or "" when refused.
  Q_INVOKABLE QString add(const QString& track_id, const QString& engine_id, int index = -1,
                          int preset = -1);
  Q_INVOKABLE void remove(const QString& track_id, const QString& slot_id);
  Q_INVOKABLE void move(const QString& track_id, const QString& slot_id, int index);
  Q_INVOKABLE void set_enabled(const QString& track_id, const QString& slot_id, bool enabled);
  // The slot's device-kit controller, held until release() (counted).
  Q_INVOKABLE QObject* controller(const QString& track_id, const QString& slot_id);
  Q_INVOKABLE void release(const QString& track_id, const QString& slot_id);

signals:
  void chainChanged();

private:
  struct Held {
    std::unique_ptr<NoteEffectAdapter> adapter;
    std::unique_ptr<NativeSynthDeviceController> controller;
    int users = 0;
  };
  [[nodiscard]] application::ProjectSession* editable_session(const QString& action);
  void accepted(const QString& message);
  void failed(const QString& action, const foundation::Error& error);

  Session session_;
  CanEdit can_edit_;
  Report report_;
  Hook after_edit_;
  Hook live_edit_;
  Hook save_;
  ControllerCreated created_;
  std::map<std::string, Held> held_; // By slot id.
};

} // namespace oscillate::quick
