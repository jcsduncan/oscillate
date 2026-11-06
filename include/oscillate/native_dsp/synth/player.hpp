#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/synth/plan_mailbox.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::native_dsp::synth {

// One instrument track's built-in synth with its MIDI, automation lanes and
// musical position. Live routed playback and offline export both render
// through this, so their timing and parameter semantics are identical.
class NativeSynthPlayer {
public:
  // Control only. `automation` is the project's prepared automation; lanes for
  // this track's native parameters are taken from it. Automated frames must
  // not pass last_safe_automation_sample (see application::automation_sample_limit).
  // `media` holds the patch document's decoded managed slots
  // (source_prep::load_synth_media).
  [[nodiscard]] static foundation::Result<NativeSynthPlayer>
  prepare(const project::Snapshot& snapshot, const project::Track& track,
          const engine::InstrumentEvents& events,
          std::span<const automation_project::PreparedTargetAutomation> automation,
          std::size_t max_frames, std::uint64_t last_safe_automation_sample, SynthMedia media = {});

  // Callback safe and additive. Fails, leaving output untouched, for blocks
  // larger than max_frames, MIDI overflow, automation beyond its safe range
  // or a rejected synth block. `workers` are audio helpers the synth may
  // spread its own work over (ADR-0085); the output does not depend on them.
  [[nodiscard]] bool render(std::span<engine::StereoFrame> output, std::uint64_t project_frame,
                            std::uint64_t transport_generation,
                            engine::AudioWorkerPool* workers = nullptr) noexcept;
  // Offline renders only, before each render: waits until the synth's
  // streamed content is ready (NativeSynthRuntime::await_content).
  void await_content() noexcept {
    if (runtime_) {
      runtime_->await_content();
    }
  }

  // Control only. The exact track's new evaluator is built off callback and
  // offered without replacing the runtime, MIDI plan, or sounding voices.
  // The plan also carries the track's note effect values and switches, so
  // an edit to them that keeps the chain's slots goes live here too.
  [[nodiscard]] foundation::Result<void>
  offer_automation(const project::Snapshot& snapshot, const project::Track& track,
                   std::span<const automation_project::PreparedTargetAutomation> automation,
                   std::uint64_t last_safe_automation_sample);
  [[nodiscard]] std::uint64_t applied_automation_revision() const noexcept;

  // Control only: Kick & Bass Lock (ADR-0072). Compiles the notes this
  // track's engine follows from the snapshot and offers them to the
  // runtime. Engines that follow nothing are untouched.
  [[nodiscard]] foundation::Result<void> refresh_lock(const project::Snapshot& snapshot);
  // Control only: the project's key, for engines that tune to it. Prepare
  // offers the snapshot's; a live key change offers the new one.
  void follow_project_key(std::optional<project::MusicalKey> key) noexcept {
    runtime_->offer_project_key(key);
  }

  // Control only, before the first callback. Replace the private MIDI plan
  // with an exact export cut: release held notes at end and omit later notes.
  [[nodiscard]] foundation::Result<void> prepare_export_end(std::uint64_t end);
  [[nodiscard]] bool export_end_prepared(std::uint64_t end) const noexcept {
    return export_end_ == end;
  }

  [[nodiscard]] project::TrackId track() const noexcept { return track_; }
  // Live control offers panel values here; shared so control can keep it alive.
  [[nodiscard]] const std::shared_ptr<NativeSynthRuntime>& runtime() const noexcept {
    return runtime_;
  }
  [[nodiscard]] const std::vector<double>& values() const noexcept { return values_; }
  [[nodiscard]] bool automated() const noexcept;
  // Frames a note released at the end of the MIDI plan keeps sounding.
  [[nodiscard]] std::uint64_t tail_frames() const noexcept { return tail_frames_; }
  // Control only (one producer): a note played live, heard from the next
  // block's start alongside the project's notes. False when the inbox is
  // full. A note-on for a key already held releases it first.
  [[nodiscard]] bool queue_live_note(std::uint8_t key, std::uint8_t velocity, bool on) noexcept;
  // True when the track's note effects have these slots and engines in
  // this order (values and switches may differ: those are live edits).
  [[nodiscard]] bool same_note_effects(std::span<const project::NoteEffectInsert> inserts) const;

private:
  explicit NativeSynthPlayer(project::TrackId track) : track_(track) {}

  struct AutomationPlan {
    std::uint64_t revision = 0;
    std::uint64_t last_safe_sample = 0;
    std::vector<std::shared_ptr<const automation_project::PreparedTargetAutomation>> evaluators;
    std::vector<std::uint32_t> parameters;
    std::vector<std::vector<double>> values;
    std::vector<ParameterLane> lanes;
    // Note effects (ADR-0059), per stage in chain order: values, switches
    // and the lanes of automated parameters (macro lanes), whose buffers
    // note_lane_values hold in note_evaluators' order.
    std::vector<std::vector<double>> note_values;
    std::vector<std::shared_ptr<const automation_project::PreparedTargetAutomation>>
        note_evaluators;
    std::vector<std::vector<double>> note_lane_values;
    std::vector<std::vector<ParameterLane>> note_lanes;
    std::vector<note_effects::StageControls> note_controls;
  };
  [[nodiscard]] foundation::Result<std::unique_ptr<AutomationPlan>>
  make_automation_plan(const project::Snapshot& snapshot, const project::Track& track,
                       std::span<const automation_project::PreparedTargetAutomation> automation,
                       std::uint64_t last_safe_automation_sample) const;

  project::TrackId track_;
  std::uint32_t sample_rate_ = 0;
  std::size_t max_frames_ = 0;
  std::shared_ptr<const engine::InstrumentEvents> events_;
  std::shared_ptr<NativeSynthRuntime> runtime_;
  std::shared_ptr<const timeline::TempoMap> tempo_;
  BarGrid bars_;
  std::vector<double> values_;
  // Boxed: the player moves, its atomics cannot.
  std::unique_ptr<PlanMailbox<AutomationPlan>> automation_mailbox_ =
      std::make_unique<PlanMailbox<AutomationPlan>>();
  std::vector<engine::BlockMidiEvent> block_events_;
  std::unique_ptr<engine::HeldNotes> held_ = std::make_unique<engine::HeldNotes>();
  // Live notes: a single-producer ring from control to the callback, and the
  // id each held key sounds under (0: up), callback only.
  struct LiveNotes {
    struct Note {
      std::uint8_t key = 0;
      std::uint8_t velocity = 0;
      bool on = false;
    };
    static constexpr std::uint32_t capacity = 64;
    std::array<Note, capacity> ring{};
    std::atomic<std::uint32_t> write{0};
    std::atomic<std::uint32_t> read{0};
    std::array<std::uint64_t, 128> sounding{};
    std::uint64_t serial = 0;
    // A block's merge, held until the block renders: a failed block leaves
    // the queue and the sounding notes as they were, so a retry loses none.
    std::array<std::uint64_t, 128> staged_sounding{};
    std::uint64_t staged_serial = 0;
    std::uint32_t staged_read = 0;
    bool staged = false;
  };
  std::unique_ptr<LiveNotes> live_ = std::make_unique<LiveNotes>();
  // Merges queued live notes at offset 0 after the plan's `count` events,
  // staging their effect on the queue until commit_live_notes().
  [[nodiscard]] std::size_t merge_live_notes(std::size_t count) noexcept;
  // After a rendered block: the staged merge takes effect.
  void commit_live_notes() noexcept;
  // The track's note effects (ADR-0059), when it has any: their events
  // run through note_scratch_ before the synth sees them.
  std::shared_ptr<note_effects::NoteEffectChain> note_effects_;
  std::vector<engine::BlockMidiEvent> note_scratch_;
  // The track's notes just after each block, for the note effects (Comp's
  // pushes see the next chord coming).
  std::vector<note_effects::AheadEvent> note_ahead_;
  // Room for a chase of every pedal ahead of the block's own pedal events.
  std::vector<engine::BlockPedalEvent> block_pedals_;
  bool rendered_ = false;
  std::uint64_t rendered_generation_ = 0;
  std::uint64_t tail_frames_ = 0;
  std::optional<std::uint64_t> export_end_;
};

} // namespace oscillate::native_dsp::synth
