#pragma once

#include "oscillate/application/control_transport_gate.hpp"
#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/application/routed_audio_source.hpp"
#include "oscillate/application/routed_playback_control.hpp"
#include "oscillate/application/routed_source_selection.hpp"
#include "oscillate/audio_io/metronome.hpp"
#include "oscillate/engine/midi_events.hpp"
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#include "oscillate/application/vst3/instrument_provider.hpp"
#include "oscillate/application/vst3/manual_effect_edit.hpp"
#include "oscillate/application/vst3/track_insert_provider.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::quick {

// A full-binary qualification is reserved for synthetic fixtures. Production
// evidence is scoped to an exact binary, class, control, rate and block size.
struct Vst3QualifiedManualParameter {
  std::string binary_sha256;
  std::string class_id;
  std::uint32_t parameter_id;
  std::uint32_t sample_rate;
  std::uint32_t block_frames;
};
struct Vst3LiveQualification {
  std::vector<std::string> fixed_latency_binaries;
  std::vector<Vst3QualifiedManualParameter> parameters;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // Evidence for one exact saved effect instance. The provider rechecks the
  // binary, state, rate and callback size before allowing an export.
  std::vector<std::pair<project::PluginInstanceId, plugins::Vst3FixedTailQualification>>
      fixed_effect_tails;
  // Session-local evidence for an exact saved instrument binary and state.
  std::vector<std::pair<project::PluginInstanceId, plugins::Vst3InstrumentTailQualification>>
      fixed_instrument_tails;
#endif
};

#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
// One generic-editor gesture captured on an independent suspended clone of the
// saved plugin opened at opened_revision. An absent insert names the track's
// instrument source.
struct Vst3ManualGesture {
  std::uint64_t opened_revision;
  project::TrackId track;
  std::optional<project::InsertSlotId> insert;
  project::Vst3PluginInstance original;
  std::vector<std::uint8_t> component_state;
  std::vector<project::Vst3ParameterReference> references;
  plugins::Vst3ParameterEvent parameter;
  std::uint64_t gesture_sequence{0}; // Zero denotes an untagged generic editor edit.
};

struct Vst3ManualGestureStatus {
  project::PluginInstanceId plugin;
  application::Vst3ManualEditStatus status;
};

// User-facing reason for a refused manual event admission.
[[nodiscard]] const char* vst3_manual_refusal(plugins::Vst3ManualEventStatus status) noexcept;
#endif

[[nodiscard]] bool has_saved_vst3_insert(const project::Snapshot& snapshot) noexcept;

// How the device callback fills a block from the source it chose: the active
// source, or a staged candidate for its own revision (`exact`). A candidate
// must fill completely; when it cannot, the block plays the active graph and
// the candidate waits for a later block. Callback only: no allocation, no
// locks. A run fills straight from the source unless given another fill.
class RoutedSourceFill {
public:
  RoutedSourceFill() = default;
  RoutedSourceFill(const RoutedSourceFill&) = delete;
  RoutedSourceFill& operator=(const RoutedSourceFill&) = delete;
  virtual ~RoutedSourceFill() = default;

  [[nodiscard]] virtual bool fill(application::RoutedAudioSource& source, bool exact,
                                  engine::BlockContext block, std::size_t frames,
                                  std::span<const std::span<const engine::StereoFrame>>& selected,
                                  engine::AudioWorkerPool* workers) noexcept;
};
[[nodiscard]] bool has_saved_track_effect(const project::Snapshot& snapshot) noexcept;

// A routed playback run with control-thread publication of project revisions.
// Every track and the master run one chain of built-in and VST3 stages
// (ADR-0050); a chain edit switches live while the old graph keeps playing,
// and a plugin new to the run is scanned on a worker first. Audio-only clip
// revisions prepare a replacement source while the old graph plays. Other
// unsupported source changes fail explicitly; a failed revision leaves the
// last graph audible.
// Construct/start and stop/retire on workers; only status observation runs on the Quick control
// sequencer. The session must outlive this run and its playback reservation. A failed preparation
// starts no output or active graph.
class RoutedPlayback final {
public:
  enum class StopDrainOutcome : std::uint8_t {
    not_requested,
    completed,
    truncated,
    instrument_release_failed
  };
  [[nodiscard]] static foundation::Result<std::unique_ptr<RoutedPlayback>> prepare_and_start(
      const std::filesystem::path& root, project::Snapshot snapshot,
      application::ControlTransportGate::PlaybackToken token,
      application::PlaybackOutputFactory output_factory, std::optional<std::uint64_t> target_serial,
      std::uint32_t requested_buffer_frames = application::default_playback_buffer_frames,
      std::uint64_t first_sample = 0, Vst3LiveQualification qualification = {},
      std::shared_ptr<audio_io::Metronome> metronome = {},
      std::shared_ptr<RoutedSourceFill> source_fill = {});
  // Prepare an independent graph/source/cursor without opening an output.
  // The caller must attach exactly one output before using the render source.
  [[nodiscard]] static foundation::Result<std::unique_ptr<RoutedPlayback>>
  prepare_for_export(const std::filesystem::path& root, project::Snapshot snapshot,
                     application::ControlTransportGate::PlaybackToken token,
                     std::uint32_t requested_buffer_frames,
                     Vst3LiveQualification qualification = {});

  RoutedPlayback(const RoutedPlayback&) = delete;
  RoutedPlayback& operator=(const RoutedPlayback&) = delete;
  ~RoutedPlayback();

  // Worker-side graceful Stop. The callback switches source slots to silence
  // at a block boundary while retaining the published master processor.
  void begin_graceful_stop() noexcept;
  // Worker-side device handoff. Stop joins the callback; graph, source rings,
  // instrument voices and processor histories remain owned by this run.
  [[nodiscard]] foundation::Result<void> pause();
  [[nodiscard]] foundation::Result<void> seek_paused(std::uint64_t first_sample);
  [[nodiscard]] foundation::Result<void> resume();
  // UI-thread control only. The source and device stay owned by this run;
  // compatible graph edits publish at an acknowledged callback boundary.
  [[nodiscard]] foundation::Result<void> submit_revision(project::Snapshot snapshot);
  [[nodiscard]] bool edit_pending() const noexcept {
    return edit_pending_ || (control_ && control_->vst3_manual_edit_pending());
  }
  // Control sequencer only. Pause/seek waits until the current graph handoff
  // has either activated or failed; source preparation alone does not block it.
  [[nodiscard]] bool graph_handoff_in_progress() const noexcept;
  [[nodiscard]] bool paused() const noexcept { return paused_.load(std::memory_order_acquire); }
  StopDrainOutcome stop_and_retire() noexcept;
  [[nodiscard]] application::RevisionStatus status();
  [[nodiscard]] audio_io::OutputStatus output_status() const noexcept;
  [[nodiscard]] application::RoutedSourceStatus source_status() const noexcept;
  [[nodiscard]] std::optional<media::UnderrunEvent> pop_source_underrun() noexcept;
  [[nodiscard]] std::optional<foundation::Error> source_failure() const;
  // Control sequencer only. Per-clip live state of processed (warped or
  // pitched) clips in the active source: cache or stream backing, quality
  // tier and step-downs. It describes the source of the active graph
  // revision, never a pending edit's candidate.
  [[nodiscard]] std::vector<application::RoutedProcessedClipStatus> processed_clip_status() const;
  [[nodiscard]] std::optional<bool> synth_deferred_changes(project::TrackId track) const noexcept;
  [[nodiscard]] bool synth_telemetry(project::TrackId track, std::vector<float>& frame);
  // Control sequencer only: loop playback over project samples [start, end).
  // The range is armed on the active source and re-armed on every source
  // promoted by an edit. set_loop() with another range and clear_loop()
  // withdraw a wrap queued for the old range; if the callback applied it
  // first, that pass simply wrapped. service_loop(), called on every control
  // poll, queues a planned wrap at `end` once the source reports the next
  // transport generation primed for the current range (the transport splits
  // the device block there, so the wrap is sample-exact), and restarts the
  // transport in place when the source reports frames queued for a range
  // that no longer applies (a withdrawn pass, or a range armed after its
  // clips were already streamed past `end` while the playhead is before it).
  // That restart is a short priming hold at the current position, not a
  // jump; it lets the next pass wrap exactly instead of seeking at the end.
  struct LoopProgress {
    bool scheduled = false;  // a wrap at the loop end is queued
    bool restarting = false; // an in-place restart is discarding stale frames
    bool missed = false;     // the next pass is not primed and will not be
    audio_io::TransportCursor::LoopWrapCounts wraps;
  };
  [[nodiscard]] bool set_loop(std::uint64_t start, std::uint64_t end);
  void clear_loop();
  [[nodiscard]] application::RoutedAudioSource::LoopStatus loop_status() const;
  [[nodiscard]] LoopProgress service_loop();
  [[nodiscard]] audio_io::TransportCursor::CompletedPosition completed_position() const noexcept {
    return cursor_.completed_snapshot();
  }
  // The last block's first sample and when it is heard (ADR-0086), if the
  // device reports its delay.
  [[nodiscard]] std::optional<audio_io::TransportCursor::AudiblePoint>
  audible_point() const noexcept {
    return cursor_.audible_point();
  }
  [[nodiscard]] std::optional<foundation::Error> processor_fault() const;
  // A fader, pan or send mid-drag, heard from the next block without a
  // saved revision (RoutedPlaybackControl::set_live_channel).
  [[nodiscard]] bool set_live_channel(std::optional<project::TrackId> track, double level,
                                      double pan) noexcept;
  [[nodiscard]] bool set_live_send(project::TrackId track, project::SendId send,
                                   double gain) noexcept;
  // A built-in synth's or effect's panel values mid-drag
  // (RoutedPlaybackControl::set_live_synth_values).
  [[nodiscard]] bool set_live_synth_values(project::TrackId track,
                                           const native_dsp::synth::EngineDescriptor& engine,
                                           std::span<const double> values) noexcept;
  // A live note on a track's synth (RoutedPlaybackControl::live_note).
  [[nodiscard]] bool live_note(project::TrackId track, std::uint8_t key, std::uint8_t velocity,
                               bool on) noexcept;
  [[nodiscard]] bool set_live_effect_values(std::optional<project::TrackId> track,
                                            project::InsertSlotId slot,
                                            const native_dsp::synth::EngineDescriptor& engine,
                                            std::span<const double> values) noexcept;
  // Control sequencer only. The track's post-fader peak/RMS in the
  // callback-accepted graph; empty for a track that graph does not contain.
  [[nodiscard]] std::optional<engine::RoutedNodeMeterReading>
  track_meter(project::TrackId track) const noexcept;
  // The running built-in effect's newest telemetry frame (ADR-0043).
  [[nodiscard]] bool effect_telemetry(std::optional<project::TrackId> track,
                                      project::InsertSlotId insert, std::vector<float>& frame);
  // Live sidechain key monitoring for a built-in effect's panel.
  [[nodiscard]] bool set_effect_key_listen(project::TrackId track, project::InsertSlotId insert,
                                           bool listen) noexcept;
  [[nodiscard]] std::optional<float> effect_key_level(project::TrackId track,
                                                      project::InsertSlotId insert) const noexcept;
  // Control sequencer only. The manual event slot of the exact saved effect or
  // instrument source in the callback-accepted graph, exposed only while that
  // graph is this run's latest revision and no edit, source handoff or pause is
  // pending, as of the last status() observation. The control re-checks the
  // publisher's live revision, so a stale observation only fails closed. The
  // slot itself enforces fixed-latency qualification.
  [[nodiscard]] std::optional<application::ActiveVst3EffectManualEvents>
  active_vst3_effect_manual_events(project::TrackId track, project::InsertSlotId insert,
                                   project::PluginInstanceId plugin) const;
  [[nodiscard]] std::optional<application::ActiveVst3InstrumentManualEvents>
  active_vst3_instrument_manual_events(project::TrackId track,
                                       project::PluginInstanceId plugin) const;
  [[nodiscard]] std::optional<application::PendingVst3ManualPreviewEvents>
  pending_vst3_manual_preview_events(project::TrackId track,
                                     std::optional<project::InsertSlotId> insert,
                                     project::PluginInstanceId plugin) const;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // Control sequencer only. Whether a gesture on this saved plugin could be
  // saved as saved_revision + 1 and made live: its exact qualified slot admits
  // the value and audible state equals the saved revision. Nothing changes.
  [[nodiscard]] foundation::Result<void>
  preflight_vst3_manual_edit(std::uint64_t saved_revision, project::TrackId track,
                             std::optional<project::InsertSlotId> insert,
                             project::PluginInstanceId plugin,
                             plugins::Vst3ParameterEvent parameter);
  // Saves the gesture as one session revision through the PI-09 transaction,
  // adopts it into this run's mirror and submits one manual event. The value
  // is live only after status() observes the exact applied receipt. The
  // session must be the source of this run's revisions and outlive it.
  [[nodiscard]] foundation::Result<application::Vst3ManualEditStatus> commit_vst3_manual_edit(
      application::ProjectSession& session, Vst3ManualGesture gesture,
      application::EditorSavePolicy save_policy = application::EditorSavePolicy::immediate);
  // Latest gesture status, as of the last status() observation.
  [[nodiscard]] std::optional<Vst3ManualGestureStatus> vst3_manual_edit_status() const;
#endif
  [[nodiscard]] std::uint64_t completed_sample() const noexcept;
  [[nodiscard]] std::uint32_t sample_rate() const noexcept;
  [[nodiscard]] std::size_t prepared_block_limit() const noexcept;

private:
  friend class RealtimeExportRun;
  [[nodiscard]] static foundation::Result<std::unique_ptr<RoutedPlayback>>
  prepare_run(const std::filesystem::path& root, project::Snapshot snapshot,
              application::ControlTransportGate::PlaybackToken token,
              std::uint32_t requested_buffer_frames, std::uint64_t first_sample,
              Vst3LiveQualification qualification);
  class StoppedGate;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  class ManualAdoption;
#endif
  RoutedPlayback(application::ControlTransportGate::PlaybackToken token,
                 std::unique_ptr<StoppedGate> gate, std::uint64_t first_sample) noexcept;

  std::optional<application::ControlTransportGate::PlaybackToken> token_;
  std::unique_ptr<StoppedGate> gate_;
  std::optional<project::ProjectModel> model_;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  std::unique_ptr<plugins::Vst3TrackInsertProvider> inserts_;
  std::unique_ptr<plugins::Vst3InstrumentTrackProvider> instruments_;
  std::unique_ptr<application::TrackInsertProvider> vst3_;
#endif
  // Declared before the output, so the output joins its workers first.
  std::unique_ptr<engine::AudioWorkerPool> workers_;
  std::unique_ptr<application::RoutedPlaybackControl> control_;
  std::shared_ptr<application::RoutedAudioSource> source_;
  std::unique_ptr<application::RoutedSourceSelection> source_selection_;
  std::filesystem::path project_root_;
  std::optional<application::RoutedSourcePreparationCapture> source_capture_;
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  Vst3LiveQualification qualification_;
  // A scanned saved plugin, ready for its provider.
  struct Vst3Scan {
    project::Vst3PluginInstance saved;
    bool instrument = false;
    foundation::Result<plugins::ScanResult> scanned;
  };
  // Records a scan's receipt and qualification with the provider that hosts
  // the plugin; a failed scan stays unadmitted, so its preparation fails.
  void admit_vst3(Vst3Scan scan);
  // Plugins whose scan receipts this run holds.
  std::vector<project::PluginInstanceId> admitted_vst3_;
  // An edit waiting for its new plugins' scans, which run on a worker while
  // the old graph keeps playing.
  std::optional<project::AppliedRevision> scanning_revision_;
  std::future<std::vector<Vst3Scan>> vst3_scans_;
#endif
  std::future<foundation::Result<std::unique_ptr<application::RoutedAudioSource>>>
      source_preparation_;
  std::vector<std::future<foundation::Result<std::unique_ptr<application::RoutedAudioSource>>>>
      abandoned_source_preparations_;
  std::shared_ptr<application::RoutedAudioSource> source_candidate_;
  // Callback reads the exact staged source revision; all other graph
  // revisions may keep using the selected active source.
  std::atomic<std::uint64_t> source_overlay_revision_{0};
  std::shared_ptr<RoutedSourceFill> source_fill_ = std::make_shared<RoutedSourceFill>();
  std::vector<std::future<void>> source_retirements_;
  bool source_hold_requested_ = false;
  bool source_staged_ = false;
  bool source_cancel_pending_ = false;
  std::uint8_t source_retry_count_ = 0;
  struct SynthSwitchPolicy {
    engine::CompiledMidiPlan old_notes;
    std::vector<std::pair<project::TrackId, std::uint64_t>> release_tails;
    std::chrono::steady_clock::time_point deadline;
  };
  std::optional<SynthSwitchPolicy> synth_switch_policy_;
  std::optional<audio_io::TransportCursor::CompletedPosition> resumed_boundary_;
  // The device-only click (ADR-0086); it outlives the cursor and output.
  std::shared_ptr<audio_io::Metronome> metronome_;
  audio_io::TransportCursor cursor_;
  std::unique_ptr<application::PlaybackOutput> output_;
  std::optional<std::uint64_t> target_serial_;
  std::uint32_t requested_buffer_frames_ = 128;
  std::atomic<bool> paused_{false};
  bool graceful_stop_requested_ = false; // serialized with stop_and_retire
  std::uint32_t requested_drain_frames_ = 0;
  bool edit_pending_ = false;
  bool edit_awaiting_activation_ = false;
  bool callback_frozen_ = false; // callback-owned
  std::optional<project::Snapshot> queued_revision_;
  struct LoopRange {
    std::uint64_t start;
    std::uint64_t end;
  };
  void withdraw_loop_wrap() noexcept;
  std::optional<LoopRange> loop_;               // control sequencer
  std::uint64_t loop_scheduled_generation_ = 0; // control sequencer
  // Completed generation when an in-place restart was queued; 0 when none.
  std::uint64_t loop_restart_generation_ = 0; // control sequencer
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
  // Declared last: a gesture's transaction settles through the adoption owner
  // and is destroyed before it and the control.
  std::unique_ptr<ManualAdoption> manual_adoption_;
  std::optional<application::Vst3ManualEffectEdit> manual_effect_;
  std::optional<application::Vst3ManualInstrumentEdit> manual_instrument_;
  std::optional<Vst3ManualGestureStatus> manual_status_;
  void observe_manual_edit();
#endif
  [[nodiscard]] foundation::Result<void> begin_revision(project::Snapshot snapshot);
  [[nodiscard]] foundation::Result<void> start_source_preparation();
#if defined(OSCILLATE_HAVE_VST3) && OSCILLATE_HAVE_VST3
#endif
  void retire_source(std::shared_ptr<application::RoutedAudioSource> source);
  void collect_source_retirements();
  void fail_source_handoff(const foundation::Error& error);
  [[nodiscard]] bool synth_switch_quiet_at(std::uint64_t sample) const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> settled_graph_revision() const;
  [[nodiscard]] bool manual_adopted(std::uint64_t revision) const;
  [[nodiscard]] audio_io::StereoRenderSource render_source() noexcept;
  static bool render(void* context, std::span<engine::StereoFrame> output,
                     engine::BlockContext block) noexcept;
  static bool freeze_cursor(void* context) noexcept;
};

} // namespace oscillate::quick
