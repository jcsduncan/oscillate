#pragma once

#include "native_synth_panel_adapter.hpp"
#include "oscillate/application/project_session.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/source_prep/managed_audio.hpp"

#include <QTimer>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace oscillate::quick {

// What the running synth has applied, as reported by routed playback.
struct AudibleSynthState {
  std::uint64_t revision = 0;
  bool deferred_changes = false;
};

// NativeSynthPanelAdapter over ProjectSession for one track playing `engine`,
// or for one built-in effect slot (ADR-0043) on a track's chain or the
// master. If the track plays another engine or none (or the slot is gone or
// holds another effect), the panel is read-only and shows the engine's
// defaults with an explanation. Root wiring
// supplies hooks so this file needs no project bridge:
//  - after_edit(revision) runs after each accepted model edit, so routed
//    playback can offer it live (RoutedPlaybackControl::begin_edit);
//  - audible() reports the running synth's acknowledged revision, or
//    nullopt when nothing is playing (then saved values are what will play).
//  - telemetry(frame) reads the running synth's newest telemetry frame,
//    or returns false when nothing is playing.
//  - live(values) offers a drag's values to the running instance with no
//    saved revision (ADR-0054); false when nothing is playing it.
//  - decode(record, stop) decodes a managed source of the project on a
//    worker, for media(); without it managed media report why they cannot
//    be shown.
// A drag saves nothing until it ends: each move is heard through live()
// and shown by values(); the release saves one edit, one undo step, and a
// cancel restores the saved values without touching the project.
// Single edits and commits save the project.
class SessionNativeSynthAdapter final : public NativeSynthPanelAdapter {
public:
  using EditHook = std::function<void(std::uint64_t revision)>;
  using AudibleProbe = std::function<std::optional<AudibleSynthState>()>;
  using TelemetryProbe = std::function<bool(std::vector<float>& frame)>;
  using SaveHook = std::function<void()>;
  using LiveHook = std::function<bool(std::span<const double> values)>;
  // Runs on a worker: no project access beyond the store it captured.
  using MediaDecoder = std::function<foundation::Result<source_prep::DecodedAudio>(
      const project::SourceRecord& record, const std::stop_token& stop)>;

  // `engine` must outlive the adapter (registered engines are static).
  SessionNativeSynthAdapter(application::ProjectSession& session, project::TrackId track,
                            const native_dsp::synth::SynthEngine& engine, EditHook after_edit = {},
                            AudibleProbe audible = {}, TelemetryProbe telemetry = {},
                            SaveHook request_save = {}, LiveHook live = {},
                            MediaDecoder decode = {});
  SessionNativeSynthAdapter(const SessionNativeSynthAdapter&) = delete;
  SessionNativeSynthAdapter& operator=(const SessionNativeSynthAdapter&) = delete;
  // Stops and joins the decode and import workers; an unfinished import
  // changes nothing.
  ~SessionNativeSynthAdapter() override;

  [[nodiscard]] std::vector<double> values() const override;
  void submit(std::span<const double> values, NativeSynthGesture gesture) override;
  [[nodiscard]] NativeSynthPanelStatus status() const override;
  [[nodiscard]] bool telemetry(std::vector<float>& frame) override;
  [[nodiscard]] project::NativeSynthDocument document() const override;
  void submit_document(project::NativeSynthDocument document, NativeSynthGesture gesture) override;
  bool import_media(std::string_view slot, std::string_view path) override;
  void submit_state(std::span<const double> values, project::NativeSynthDocument document,
                    NativeSynthGesture gesture) override;
  [[nodiscard]] NativeSynthMedia media(std::string_view slot) override;
  void set_media_listener(std::function<void()> listener) override;
  bool begin_media_import(std::string_view slot, std::string_view path,
                          NativeSynthMediaPlanner plan, NativeSynthMediaDone done) override;
  [[nodiscard]] bool media_import_busy() const override { return import_ != nullptr; }
  bool add_midi_clip(std::int64_t bar, timeline::TickPosition length,
                     std::vector<project::MidiNote> notes) override;
  [[nodiscard]] std::vector<std::string> midi_clip_ids() const override;
  [[nodiscard]] std::optional<NativeSynthClipWindow>
  first_clip_window(std::int64_t bars) const override;
  bool replace_midi_clip(std::string_view clip, timeline::TickPosition length,
                         std::vector<project::MidiNote> notes) override;
  bool submit_preset(std::size_t preset) override;
  [[nodiscard]] std::optional<std::string> preset_text() const override;
  bool submit_preset_text(std::string_view text) override;
  [[nodiscard]] std::size_t linked_count() const override;
  void unlink() override;
  [[nodiscard]] std::optional<std::uint32_t> macro_of(std::string_view parameter_id) const override;
  bool map_to_macro(std::string_view parameter_id, std::uint32_t index, double minimum,
                    double maximum) override;
  bool unmap_macro(std::string_view parameter_id) override;
  [[nodiscard]] std::vector<std::pair<std::string, std::uint32_t>> macro_bindings() const override;
  // A macro turning elsewhere (MacroController): values() shows these dense
  // values, unsaved, until called with null. Ignored during this panel's own
  // drag.
  void show_macro_values(const std::vector<double>* values);

  // A built-in effect slot. `effect` must outlive the adapter.
  SessionNativeSynthAdapter(application::ProjectSession& session, project::SignalPathId path,
                            project::InsertSlotId slot,
                            const native_dsp::effects::EffectEngine& effect,
                            EditHook after_edit = {}, AudibleProbe audible = {},
                            TelemetryProbe telemetry = {}, SaveHook request_save = {},
                            LiveHook live = {});

  // The synth track, or the effect slot's track (see path() on the master).
  [[nodiscard]] project::TrackId track() const noexcept { return track_; }
  // The synth engine; only for synth adapters (effect_slot() is empty).
  [[nodiscard]] const native_dsp::synth::SynthEngine& engine() const noexcept { return *engine_; }
  [[nodiscard]] const native_dsp::EngineDescriptor& descriptor() const noexcept {
    return descriptor_;
  }
  [[nodiscard]] std::optional<project::InsertSlotId> effect_slot() const noexcept { return slot_; }
  [[nodiscard]] const project::SignalPathId& path() const noexcept { return path_; }

private:
  [[nodiscard]] static project::TrackId path_track(const project::SignalPathId& path);
  [[nodiscard]] foundation::Result<std::vector<double>> saved_values() const;
  [[nodiscard]] foundation::Result<void> write_values(std::span<const double> values,
                                                      std::optional<std::uint64_t> gesture);
  // Why this track cannot be edited with this engine, or empty when it can.
  [[nodiscard]] std::string unavailable_reason() const;
  void apply(std::span<const double> values, std::optional<std::uint64_t> gesture);
  void apply_document(project::NativeSynthDocument document, std::optional<std::uint64_t> gesture);
  // After an accepted one-step edit: offer it to playback and save.
  void accepted(std::uint64_t before);
  void save();
  void apply_state(std::span<const double> values, project::NativeSynthDocument document,
                   std::optional<std::uint64_t> gesture);
  // The track's synth patch as saved, or null when unavailable.
  [[nodiscard]] const project::NativeInstrumentPatch* saved_patch() const;
  // Polls the workers on the UI thread and lands what they finished.
  void poll();
  void finish_decode();
  void finish_import();
  void remember(std::string identity,
                std::shared_ptr<const native_dsp::synth::SynthMediaAsset> audio);
  [[nodiscard]] std::shared_ptr<const native_dsp::synth::SynthMediaAsset>
  remembered(std::string_view identity) const;
  // The macro destination of one of this device's parameters; none on the
  // master, whose effects belong to no track's macros.
  [[nodiscard]] std::optional<project::MacroDestination>
  macro_destination(std::string_view parameter_id) const;

  application::ProjectSession& session_;
  project::TrackId track_;
  project::SignalPathId path_;
  std::optional<project::InsertSlotId> slot_;
  const native_dsp::synth::SynthEngine* engine_ = nullptr;
  const native_dsp::effects::EffectEngine* effect_ = nullptr;
  const native_dsp::EngineDescriptor& descriptor_;
  EditHook after_edit_;
  AudibleProbe audible_;
  TelemetryProbe telemetry_;
  SaveHook request_save_;
  LiveHook live_;
  // Panel gestures use their own range so they never merge with other edits.
  std::uint64_t next_gesture_ = 0x5052'0000'0000'0001ULL;
  std::optional<std::uint64_t> gesture_;
  // A drag's values, heard and shown but not yet saved.
  std::optional<std::vector<double>> live_values_;
  // The document from before a document drag began, for cancel.
  std::optional<project::NativeSynthDocument> document_before_gesture_;
  // The values from before a state drag began, for cancel.
  std::optional<std::vector<double>> values_before_gesture_;
  std::string message_;

  // Decoded media by identity (sha256), newest last, at most
  // max_remembered_media: a re-chop or a waveform redraw never re-decodes,
  // and undoing to the previous sample finds it still here.
  struct RememberedMedia {
    std::string identity;
    std::shared_ptr<const native_dsp::synth::SynthMediaAsset> audio;
  };
  std::vector<RememberedMedia> remembered_;
  MediaDecoder decode_;
  std::function<void()> media_listener_;
  // One decode at a time, written by its worker until `done`.
  struct Decode {
    std::atomic<bool> done{false};
    std::string identity;
    std::optional<foundation::Result<source_prep::DecodedAudio>> result;
  };
  std::shared_ptr<Decode> decode_job_;
  std::string decode_failure_identity_;
  std::string decode_failure_;
  // One import at a time, written by its worker until `done`.
  struct Import {
    std::atomic<bool> done{false};
    std::optional<foundation::Result<application::PreparedNativeSynthMediaImport>> prepared;
    std::shared_ptr<const native_dsp::synth::SynthMediaAsset> audio;
    std::optional<foundation::Result<NativeSynthMediaPlan>> plan;
    NativeSynthMediaDone done_callback;
  };
  std::shared_ptr<Import> import_;
  QTimer poll_timer_;
  // Declared last: joined (stop requested) before the jobs they write go.
  std::jthread decode_worker_;
  std::jthread import_worker_;
};

} // namespace oscillate::quick
