#pragma once

#include "oscillate/application/revision_status.hpp"
#include "oscillate/engine/routed_publisher.hpp"
#include "oscillate/native_dsp/convolution_ir.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_inserts/effect_stage.hpp"
#include "oscillate/plugins/vst3_graph_node.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/routing/project_route_plan.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::native_dsp {
class ConvolutionRuntime;
} // namespace oscillate::native_dsp
namespace oscillate::native_dsp::synth {
class NativeSynthPlayer;
}
namespace oscillate::persistence {
class ProjectStore;
}
namespace oscillate::audio_io {
class TransportCursor;
}

namespace oscillate::application {
class RoutedAudioSource;

// Supplied only after the device/whole graph has measured free callback time.
// The store and budget remain control-side; neither is read by render_block.
struct LiveConvolutionAdmission {
  const persistence::ProjectStore* store = nullptr;
  native_dsp::ConvolutionIrBudget budget;
};

// A verified, activated, single-use plugin binding prepared off the callback.
// Its owner is retained for topology edits; process_failed is polled only by
// control/UI code and must be safe while the callback runs. The source kind
// must match the saved track before publication.
struct PreparedTrackInsert {
  std::shared_ptr<engine::ProcessorRuntime> processor{};
  std::uint32_t latency_frames = 0;
  std::function<bool()> process_failed{};
  bool instrument_source = false;
  // The provider owns the VST3 implementation. This read-only control-side
  // probe avoids making the application library depend on VST3 runtime code.
  std::shared_ptr<plugins::Vst3ManualEventSlot> manual_events{};
  std::function<plugins::Vst3ActiveGraphIdentity()> active_manual_graph{};
  std::optional<std::uint32_t> finite_tail_frames = std::nullopt;
  // A finite plugin declaration alone does not prove that its bound stays
  // fixed while exporting. Only an independently qualified provider sets this.
  bool fixed_tail_qualified = false;
  bool fixed_latency_qualified = false;
  // Control-side query of this exact prepared instrument runtime's MIDI cut.
  // An absent or different end frame rejects real-time export before JACK.
  std::function<std::optional<std::uint64_t>()> prepared_export_end{};
  // Set only from a loaded effect host whose exact stereo auxiliary input was
  // activated. Saved sends and scan metadata cannot assert this capability.
  bool routed_sidechain = false;
};

struct ActiveVst3EffectManualEvents {
  std::shared_ptr<plugins::Vst3ManualEventSlot> slot;
  plugins::Vst3ActiveGraphIdentity identity;
};

struct ActiveVst3InstrumentManualEvents {
  std::shared_ptr<plugins::Vst3ManualEventSlot> slot;
  plugins::Vst3ActiveGraphIdentity identity;
};

struct PendingVst3ManualPreviewEvents {
  std::shared_ptr<plugins::Vst3ManualEventSlot> slot;
  plugins::Vst3ActiveGraphIdentity identity;
};

// A copy of the exact, privately prepared initial graph, either before its
// first callback or after that same revision becomes active. Later graph
// revisions require a newly prepared control instance.
// The export module may map NodeTail to its own RealtimeNodeTail without an
// application -> export target dependency. All fields refer to one revision.
struct PreparedRouteTailReceipt {
  struct NodeTail {
    engine::RoutingNodeId node;
    std::optional<std::uint64_t> frames;
  };
  engine::RoutingLatencyPlan route;
  engine::RoutingNodeId master;
  std::uint64_t revision;
  std::vector<NodeTail> nodes;
};

// The largest block a VST3 host processes at once. A graph with larger
// blocks feeds its VST3 stages in chunks of this size.
inline constexpr std::uint32_t vst3_host_block_frames = 128;

class TrackInsertProvider {
public:
  virtual ~TrackInsertProvider() = default;
  // Prepares one instance of `saved`, a VST3 instrument or effect in
  // `snapshot`, bound to that snapshot's tempo, automation and sidechains.
  // Every call yields a new, independent instance.
  [[nodiscard]] virtual foundation::Result<PreparedTrackInsert>
  prepare(const project::Snapshot& snapshot, const project::Vst3PluginInstance& saved,
          std::uint32_t max_frames) = 0;
};

// A running built-in instrument's player that a source edit leaves alone:
// the replacement source plays it on from the block it takes over, voices
// and all.
struct KeptPlayer {
  project::TrackId track;
  std::shared_ptr<native_dsp::synth::NativeSynthPlayer> player;
};

// Immutable control-side capture for preparing a source-owned edit on
// a worker. It owns the accepted project snapshot and copies the active source
// layout/order so preparation need not read mutable control state.
struct RoutedSourcePreparationCapture {
  std::shared_ptr<const project::Snapshot> snapshot;
  std::vector<routing::ProjectRouteSource> source_layout;
  std::vector<engine::RoutingNodeId> topological_order;
  std::size_t max_frames = 0;
  std::uint64_t active_graph_revision = 0;
  bool native_synth_switch = false;
  bool source_overlay_eligible = false;
  std::vector<KeptPlayer> kept_players;
};

// Callback blocks (not frames) the playing graph may render while a live
// switch's candidate waits before the control withdraws it as one that can
// never be adopted. A
// candidate takes over at the first block it can, so even a few passed
// blocks mean it cannot; the margin covers a source bank that is briefly
// late.
inline constexpr std::uint64_t live_candidate_passed_block_limit = 64;

// Headless live-routing seam. The caller renders one source span per prepared
// topological node; source_layout() identifies external source slots. A bound
// MIDI instrument generates its own frames on its instrument-track node.
// Every edit switches at a block boundary without a hold; the transport never
// stops for one (ADR-0094).
// All control calls are serialized off the callback; render_block is callback-only.
// Bounds the last callback sample for the shared musical-time evaluator, for
// every live automation plan (synths, effects). Compute off the callback so
// extreme tempo maps fail before publication.
[[nodiscard]] foundation::Result<std::uint64_t>
automation_sample_limit(const project::Snapshot& snapshot, std::size_t max_frames);

class RoutedPlaybackControl final {
public:
  // The optional provider must outlive this control while a fresh binding or
  // retry can be requested. The prepared runtime is owned by this control and
  // the published graphs, independently of the provider after preparation.
  RoutedPlaybackControl(project::ProjectModel& model, engine::RoutedMixerLimits limits,
                        TrackInsertProvider* inserts = nullptr,
                        LiveConvolutionAdmission convolution = {});
  RoutedPlaybackControl(const RoutedPlaybackControl&) = delete;
  RoutedPlaybackControl& operator=(const RoutedPlaybackControl&) = delete;

  // Initial publication. A newer edit calls begin_edit, or for a source
  // edit capture_source_edit and then publish_captured_source.
  [[nodiscard]] foundation::Result<void> submit_current();
  // Bind the source bank after initial preparation and before starting audio.
  // It remains owned by the playback run until this control is destroyed.
  void bind_live_source(RoutedAudioSource& source) noexcept;
  // Call after the callback has joined and before the source bank is retired.
  void unbind_live_source() noexcept;
  // Callback and graph publisher must be quiescent. Resetting a long IR in the
  // callback would cause an unbounded deadline spike.
  [[nodiscard]] foundation::Result<void> reset_convolution_after_quiesce(std::uint64_t generation);
  // Device owner first joins the callback. This advances the cursor and resets
  // the active IR to the same new processor generation before output resumes.
  // A pure pause or planned loop wrap must not call this method.
  [[nodiscard]] foundation::Result<void>
  seek_convolution_after_quiesce(audio_io::TransportCursor& cursor, std::uint64_t first_sample);
  // Offers the model's newest saved revision to playback. One made while an
  // earlier switch is still settling is accepted and waits; observe() then
  // applies the newest revision, so a burst of edits coalesces onto its last.
  [[nodiscard]] foundation::Result<void> begin_edit(const project::AppliedRevision& applied,
                                                    std::string_view command_label);
  // Accepts the saved revision as failed for `error`, found before any graph
  // work (a plugin that could not be scanned). The old graph keeps playing.
  [[nodiscard]] foundation::Result<void> reject_edit(const project::AppliedRevision& applied,
                                                     std::string_view command_label,
                                                     foundation::Error error);
  // Capture accepts the saved revision. Only unchanged graphs with audio clip
  // edits (placement, trim, fades, saved warp/pitch processing) or one exact
  // native-synth engine switch are eligible; every instrument the edit leaves
  // alone is kept (kept_players). Sources and track layout stay fixed. A
  // worker prepares the replacement source and primes it for a future block.
  [[nodiscard]] foundation::Result<RoutedSourcePreparationCapture>
  capture_source_edit(const project::AppliedRevision& applied, std::string_view command_label);
  // The caller has staged the primed replacement source for the capture's
  // revision. Publishes the graph that reads it as a live switch: the
  // callback adopts both at the first block the source fills completely,
  // and the old graph and source play until then (ADR-0094).
  [[nodiscard]] foundation::Result<void>
  publish_captured_source(const RoutedSourcePreparationCapture& capture);
  // The capture's published switch was withdrawn (its source never filled):
  // the revision is pending again for a freshly primed source.
  [[nodiscard]] foundation::Result<void>
  restart_captured_source(const RoutedSourcePreparationCapture& capture);
  // Source-only, no-hold publication for one managed dry WAV clip move/trim.
  // Caller stages a separately prepared exact candidate before offering it.
  [[nodiscard]] foundation::Result<void>
  publish_captured_source_overlay(const RoutedSourcePreparationCapture& capture);
  [[nodiscard]] foundation::Result<void>
  fail_captured_source_edit(const RoutedSourcePreparationCapture& capture, foundation::Error error);
  [[nodiscard]] bool can_capture_source_edit(const project::Snapshot& candidate) const;
  [[nodiscard]] foundation::Result<void> retry();
  // Also withdraws a live switch's candidate the callback has passed over for
  // live_candidate_passed_block_limit blocks: the old graph plays on, the
  // candidate's revision is reported failed and the next edit publishes.
  void observe();

  [[nodiscard]] RevisionStatus status() const { return revisions_.snapshot(); }
  // Control thread only. Accepts the initial, never-rendered pending graph so
  // dedicated JACK export can preflight before opening a device. Refuses a
  // stale model, graph transition, live edit, unbounded source or processor.
  [[nodiscard]] foundation::Result<PreparedRouteTailReceipt>
  prepared_route_tail_receipt(std::uint64_t expected_revision,
                              std::optional<std::uint64_t> export_end = std::nullopt) const;
  // Runtime fault is separate from graph-build application status: the graph
  // remains active, while the plugin adapter mutes its own failed route.
  [[nodiscard]] std::optional<foundation::Error> processor_fault() const;
  [[nodiscard]] std::optional<foundation::Error> processor_fault(project::TrackId track) const;
  [[nodiscard]] std::span<const routing::ProjectRouteSource> source_layout() const noexcept {
    return sources_;
  }
  // First track whose instrument source was prepared, if any.
  // Use the exact track predicate when a project has multiple sources.
  [[nodiscard]] std::optional<project::TrackId> bound_instrument_track() const noexcept;
  // Control side only: this exact saved source owns a prepared graph path.
  [[nodiscard]] bool has_bound_instrument_track(project::TrackId track) const noexcept;
  // Control-thread lookup for the exact saved effect in the callback-accepted
  // graph. A pending/failed model edit, pending graph, retired slot, instrument
  // source, or mismatched owner/slot/plugin/revision has no binding.
  [[nodiscard]] std::optional<ActiveVst3EffectManualEvents>
  active_vst3_effect_manual_events(project::TrackId track, project::InsertSlotId insert,
                                   project::PluginInstanceId plugin,
                                   std::uint64_t graph_revision) const;
  // The saved instrument source must still match the callback-accepted graph.
  // A provider exposes this binding only after independent fixed-latency
  // qualification; an unqualified or retired slot has no binding.
  [[nodiscard]] std::optional<ActiveVst3InstrumentManualEvents>
  active_vst3_instrument_manual_events(project::TrackId track, project::PluginInstanceId plugin,
                                       std::uint64_t graph_revision) const;
  // Preview-only access while one exact manual revision awaits its callback
  // receipt. A later gesture may be heard after that commit, but cannot save
  // or claim a live revision until the first receipt is settled.
  [[nodiscard]] std::optional<PendingVst3ManualPreviewEvents>
  pending_vst3_manual_preview_events(project::TrackId track,
                                     std::optional<project::InsertSlotId> insert,
                                     project::PluginInstanceId plugin) const;
  // Adopts one saved manual VST3 parameter revision for an active, qualified
  // effect or instrument source without rebuilding topology. The candidate must
  // be the model's current snapshot, the audible state must match the previous
  // saved revision, and the only change must be that plugin's opaque component
  // state and parameter references. The expected identity must be the plugin's
  // slot in the active topology. Call before submitting the event. The same
  // runtime, slot, DSP history and latency stay active; status stays pending
  // and later edits are refused until settle_vst3_manual_edit.
  // The same audible-state and identity checks, before the project is saved.
  [[nodiscard]] foundation::Result<void>
  preflight_vst3_manual_edit(project::PluginInstanceId plugin,
                             plugins::Vst3ActiveGraphIdentity expected) const;
  [[nodiscard]] foundation::Result<void>
  begin_vst3_manual_edit(const project::AppliedRevision& applied, std::string_view command_label,
                         project::PluginInstanceId plugin,
                         plugins::Vst3ActiveGraphIdentity expected);
  // Reports the slot's terminal receipt for the pending manual revision. An
  // applied receipt advances the audible revision and the active saved plugin.
  // A failed receipt keeps the prior audible revision and runtime, marks the
  // saved revision failed and leaves the plugin without a manual binding until
  // a fresh preparation, because audible and saved parameters now differ.
  [[nodiscard]] foundation::Result<void>
  settle_vst3_manual_edit(plugins::Vst3ManualEventReceipt receipt);
  [[nodiscard]] bool vst3_manual_edit_pending() const noexcept {
    return pending_manual_.has_value();
  }
  // Callback-accepted topology revision. It identifies retained runtimes and
  // manual slots; audible saved state (status()) may be newer after live
  // parameter adoption.
  [[nodiscard]] std::uint64_t active_topology_revision() const noexcept {
    return publisher_.active_revision();
  }
  // Control-side read of the newest telemetry frame of the built-in effect
  // running for `insert` on `track`'s chain (or the master's). The stage is
  // found by its slot identity, so a live value edit keeps it readable.
  // False when no prepared stage holds that slot.
  [[nodiscard]] bool active_effect_telemetry(std::optional<project::TrackId> track,
                                             project::InsertSlotId insert,
                                             std::vector<float>& frame);
  // Control side, live monitoring of a built-in effect's sidechain key: hear
  // it in place of the effect's output, and read its held peak. False or
  // empty when no prepared stage holds that slot.
  [[nodiscard]] bool set_active_effect_key_listen(project::TrackId track,
                                                  project::InsertSlotId insert,
                                                  bool listen) noexcept;
  [[nodiscard]] std::optional<float>
  active_effect_key_peak(project::TrackId track, project::InsertSlotId insert) const noexcept;
  // Control-side post-fader meter of the track's node in the callback-accepted
  // graph. A track absent from that graph, or before its first complete
  // window, has no reading. Node IDs are stable per track for this control.
  [[nodiscard]] std::optional<engine::RoutedNodeMeterReading>
  active_track_meter(project::TrackId track) const noexcept;
  // A channel's level and pan (the master's when `track` is empty), or a
  // send's gain, mid-gesture: heard from the next block, without a saved
  // revision. The gesture's release saves the value as an ordinary edit,
  // which then changes nothing audible. False when the channel or send is
  // not in the playing graph or the mailbox is full.
  [[nodiscard]] bool set_live_channel(std::optional<project::TrackId> track, double level,
                                      double pan) noexcept;
  [[nodiscard]] bool set_live_send(project::TrackId track, project::SendId send,
                                   double gain) noexcept;
  // A built-in synth's or effect's panel values mid-drag, in its engine's
  // table order: offered to the running instance with no saved revision,
  // gliding as panel jumps do. False when no running instance of that
  // engine plays the track or slot, the mailbox is full, or (for an effect)
  // the values would change its latency, which needs the saved edit.
  [[nodiscard]] bool set_live_synth_values(project::TrackId track,
                                           const native_dsp::synth::EngineDescriptor& engine,
                                           std::span<const double> values) noexcept;
  // A note played live on a track's built-in synth, heard through the
  // track's chain, sends and the master. False when no synth plays the
  // track or its inbox is full.
  [[nodiscard]] bool live_note(project::TrackId track, std::uint8_t key, std::uint8_t velocity,
                               bool on) noexcept;
  [[nodiscard]] bool set_live_effect_values(std::optional<project::TrackId> track,
                                            project::InsertSlotId slot,
                                            const native_dsp::synth::EngineDescriptor& engine,
                                            std::span<const double> values) noexcept;
  [[nodiscard]] std::span<const engine::RoutingNodeId> topological_order() const noexcept {
    return topological_order_;
  }
  [[nodiscard]] std::size_t max_frames() const noexcept { return limits_.max_frames; }
  // Prepared total route and terminal master latency. The engine stream emits project sample
  // N at stream frame N + this many frames, before additional device latency.
  // This is not an edge PDC budget. Stop owners must account for its tail.
  [[nodiscard]] std::uint32_t master_output_latency_frames() const noexcept {
    return master_output_latency_frames_;
  }
  // Stop drains this exact prepared graph offset when it contains a built-in
  // lookahead stage (a limiter). A failed saved removal cannot change the
  // runtime Stop contract.
  [[nodiscard]] std::optional<std::uint32_t> prepared_limiter_drain_frames() const noexcept {
    if (!prepared_limiter_)
      return std::nullopt;
    return master_output_latency_frames_;
  }
  // Control-side preparation may use only this exact accepted snapshot.
  [[nodiscard]] std::shared_ptr<const project::Snapshot> current_snapshot() const noexcept {
    return model_.snapshot();
  }
  // Control sequencer only. A transport restart cannot cross a pending
  // publication because its boundary belongs to the old generation.
  [[nodiscard]] bool graph_handoff_in_progress() const noexcept {
    return publisher_.has_pending() || pending_manual_.has_value() ||
           deferred_revision_.has_value();
  }
  [[nodiscard]] engine::RoutedRenderStatus
  render_block(std::span<const std::span<const engine::StereoFrame>> sources,
               std::span<engine::StereoFrame> output, engine::BlockContext context) noexcept {
    return publisher_.render_block(sources, output, context);
  }
  [[nodiscard]] engine::RoutedRenderStatus
  render_block(std::span<const std::span<const engine::StereoFrame>> sources,
               std::span<engine::StereoFrame> output, engine::BlockContext context,
               engine::RoutedSourceFill source_fill,
               engine::AudioWorkerPool* workers = nullptr) noexcept {
    return publisher_.render_block(sources, output, context, source_fill, workers);
  }

private:
  [[nodiscard]] native_inserts::NativeEffectStage*
  active_effect_stage(std::optional<project::TrackId> track,
                      project::InsertSlotId insert) const noexcept;
  // A running VST3 stage's host record (ADR-0050).
  struct Vst3Stage {
    // Absent on the master.
    std::optional<project::TrackId> track;
    // Absent for the track's instrument.
    std::optional<project::InsertSlotId> slot;
    project::Vst3PluginInstance saved;
    PreparedTrackInsert prepared;
  };
  // A track's running chain or, with no track, the master's (ADR-0050).
  struct ActiveChain {
    std::optional<project::TrackId> track;
    std::optional<project::InsertSlotId> sidechain_target;
    track_chain::PreparedChain chain;
  };
  [[nodiscard]] const ActiveChain* active_chain(std::optional<project::TrackId> track) const;
  [[nodiscard]] ActiveChain* active_chain(std::optional<project::TrackId> track);
  // The running stage for `insert` on `track`'s chain (or the master's).
  [[nodiscard]] track_chain::Stage* active_stage(std::optional<project::TrackId> track,
                                                 project::InsertSlotId insert);
  // A linked effect group's edit: one revision offered to a stage on each
  // of several chains (ADR-0058).
  struct PendingGroupStage {
    project::TrackId track;
    project::InsertSlotId slot;
    native_inserts::NativeEffectStage* stage = nullptr;
  };
  struct PendingNativeRevision {
    std::variant<native_dsp::synth::NativeSynthRuntime*, native_dsp::synth::NativeSynthPlayer*,
                 native_inserts::NativeEffectStage*>
        runtime;
    std::optional<project::TrackId> track;
    std::vector<project::EffectInsert> effects;
    std::shared_ptr<const project::Snapshot> snapshot;
    // Non-empty for a group edit: every stage offered, acknowledged when
    // all have applied the revision (runtime and track are the first).
    std::vector<PendingGroupStage> group;
  };
  struct PendingManualRevision {
    std::shared_ptr<const project::Snapshot> snapshot;
    project::PluginInstanceId plugin;
    plugins::Vst3ActiveGraphIdentity identity;
  };
  // The VST3 stage ready for a manual edit, when audible state equals the
  // tracker's saved revision and the slot identity matches.
  [[nodiscard]] foundation::Result<const Vst3Stage*>
  manual_slot_ready(project::PluginInstanceId plugin,
                    plugins::Vst3ActiveGraphIdentity expected) const;
  [[nodiscard]] bool published_snapshot_audible() const;
  [[nodiscard]] foundation::Result<void> begin_current();
  [[nodiscard]] foundation::Result<bool>
  try_queue_live_native(std::shared_ptr<const project::Snapshot> snapshot);
  [[nodiscard]] foundation::Result<bool>
  try_queue_live_effect_group(std::shared_ptr<const project::Snapshot> snapshot);
  [[nodiscard]] foundation::Result<bool>
  try_queue_live_master_native(std::shared_ptr<const project::Snapshot> snapshot);
  [[nodiscard]] foundation::Result<bool>
  try_queue_live_synth(std::shared_ptr<const project::Snapshot> snapshot);
  // Built-in instrument tracks' notes, lanes and note effects, offered to
  // their running players as one plan each (ADR-0093).
  [[nodiscard]] foundation::Result<bool>
  try_queue_live_players(std::shared_ptr<const project::Snapshot> snapshot);
  // Applies the model's newest saved revision by the first path that takes
  // it, or defers it while an earlier switch is still settling.
  [[nodiscard]] foundation::Result<void> apply_current();
  // An isolated lane edit on one built-in effect slot, offered to its running
  // stage without a rebuild (ADR-0043).
  [[nodiscard]] foundation::Result<bool>
  try_queue_live_effect_automation(std::shared_ptr<const project::Snapshot> snapshot);
  // The initial graph, or a live switch to `prepared_snapshot`'s. With
  // `replaces_source`, a staged replacement source carries its audio clips
  // or instrument change.
  [[nodiscard]] foundation::Result<void>
  build_and_publish(std::shared_ptr<const project::Snapshot> prepared_snapshot,
                    bool replaces_source = false);
  // Kick & Bass Lock (ADR-0072): every live synth re-reads the notes it
  // follows from the published snapshot, so a bass edit or the other
  // track's Lock switch reaches a kept player; and hears its key. Control
  // only.
  void refresh_synth_locks();
  [[nodiscard]] foundation::Result<void> fail(std::uint64_t revision, foundation::Error error);
  // Effects that read notes ahead and every live synth's Lock follow the
  // published snapshot. Control only.
  void follow_published_notes();
  // Effects that read their keys' notes ahead (Pump's MIDI trigger) take
  // the published snapshot's keys: a mute, solo or key gain changes which
  // keys sound. Control only.
  void refresh_effect_key_notes();
  // The control state a live switch replaced at publication, restored when
  // its candidate is withdrawn or discarded instead of adopted.
  struct LiveCandidate {
    std::uint64_t revision = 0;
    std::uint64_t passed_at_publication = 0;
    bool withdrawing = false;
    std::shared_ptr<const project::Snapshot> published_snapshot;
    std::shared_ptr<const project::Snapshot> source_snapshot;
    std::optional<routing::ProjectRoutePlan> route_plan;
    std::shared_ptr<native_dsp::ConvolutionRuntime> convolution;
    std::vector<ActiveChain> chains;
    std::vector<Vst3Stage> vst3_stages;
    std::vector<std::pair<project::Vst3ParameterTarget, std::uint64_t>> vst3_automation;
    bool prepared_limiter = false;
    std::uint32_t master_output_latency_frames = 0;
  };
  void settle_live_candidate();

  project::ProjectModel& model_;
  engine::RoutedMixerLimits limits_;
  routing::ProjectRouteIdRegistry registry_;
  TrackInsertProvider* inserts_;
  LiveConvolutionAdmission convolution_;
  std::shared_ptr<native_dsp::ConvolutionRuntime> live_convolution_;
  // Every running track chain and the master's. A chain edit switches only
  // the chains it changes, keeping each unchanged running stage; a built-in
  // effect's value edit reaches its running stage through a block mailbox.
  std::vector<ActiveChain> active_chains_;
  std::vector<Vst3Stage> vst3_stages_;
  std::vector<std::pair<project::Vst3ParameterTarget, std::uint64_t>> active_vst3_automation_;
  engine::RoutedMixerPublisher publisher_;
  RevisionTracker revisions_;
  std::shared_ptr<const project::Snapshot> source_snapshot_;
  std::shared_ptr<const project::Snapshot> published_snapshot_;
  std::optional<PendingNativeRevision> pending_native_revision_;
  std::optional<PendingManualRevision> pending_manual_;
  std::optional<LiveCandidate> live_candidate_;
  RoutedAudioSource* live_source_ = nullptr;
  // The players a pending instrument edit was offered to (its runtime is the
  // first); the revision is applied when all have rendered it.
  std::vector<native_dsp::synth::NativeSynthPlayer*> pending_players_;
  // The saved revision that waits for an earlier switch to settle;
  // observe() applies it then, if it is still the current, pending one.
  std::optional<std::uint64_t> deferred_revision_;
  std::uint32_t master_output_latency_frames_ = 0;
  bool prepared_limiter_ = false;
  std::shared_ptr<const project::Snapshot> preparing_source_;
  std::vector<routing::ProjectRouteSource> sources_;
  std::vector<engine::RoutingNodeId> topological_order_;
  std::optional<routing::ProjectRoutePlan> initial_route_plan_;
  std::optional<routing::ProjectRoutePlan> published_route_plan_;
  std::uint64_t initial_route_revision_ = 0;
};

} // namespace oscillate::application
