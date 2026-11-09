#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/note_effects/note_effect.hpp"
#include "oscillate/native_dsp/synth/plan_mailbox.hpp"
#include "oscillate/native_dsp/synth/runtime.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/bar_grid.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace oscillate::native_inserts {

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
          std::size_t max_frames, std::uint64_t last_safe_automation_sample,
          native_dsp::synth::SynthMedia media = {});

  // Callback safe and additive. Fails, leaving output untouched, for blocks
  // larger than max_frames, MIDI overflow, automation beyond its safe range
  // or a rejected synth block. `workers` are audio helpers the synth may
  // spread its own work over (ADR-0085); the output does not depend on them.
  [[nodiscard]] bool render(std::span<engine::StereoFrame> output, std::uint64_t project_frame,
                            std::uint64_t transport_generation,
                            engine::AudioWorkerPool* workers = nullptr) noexcept;
  // Offline renders only, before each render: waits until the synth's
  // streamed content is ready (native_dsp::synth::NativeSynthRuntime::await_content).
  void await_content() noexcept {
    if (runtime_) {
      runtime_->await_content();
    }
  }

  // Control only. An edit made while playing: the track's notes, pedals,
  // automation lanes and note effect values and switches from `snapshot`,
  // built off the callback and offered without replacing the runtime or its
  // voices. The callback adopts the plan at its next block. A sounding note
  // the new plan still holds keeps playing to its new end; one it no longer
  // holds is released at that block's start; a pedal the edit changed takes
  // its new value there. The note effects must keep their chain's slots.
  [[nodiscard]] foundation::Result<void>
  offer_plan(const project::Snapshot& snapshot, const project::Track& track,
             const engine::InstrumentEvents& events,
             std::span<const automation_project::PreparedTargetAutomation> automation,
             std::uint64_t last_safe_automation_sample);
  // offer_plan() in two steps, so one edit can reach several players or
  // none: prepare_plan() builds and checks the plan without touching the
  // player; commit_plan() offers it and cannot fail once has_room() is true.
  class PreparedPlan;
  [[nodiscard]] foundation::Result<PreparedPlan>
  prepare_plan(const project::Snapshot& snapshot, const project::Track& track,
               const engine::InstrumentEvents& events,
               std::span<const automation_project::PreparedTargetAutomation> automation,
               std::uint64_t last_safe_automation_sample) const;
  void commit_plan(PreparedPlan plan, const project::Snapshot& snapshot);
  // Control only: holds a prepared plan back for a live switch of its
  // revision, which releases it (release_staged_plan) on the callback at the
  // block it takes over (ADR-0098). False when the mailbox has no room.
  [[nodiscard]] bool stage_plan(PreparedPlan plan);
  // Callback, on the block a replacement takes over: a replaced instrument
  // rings out with the Kick & Bass Lock it had; refresh_lock() leaves it
  // alone from then on. A switch that never takes over leaves it playing.
  void retire() noexcept { retired_->store(true, std::memory_order_relaxed); }
  // Callback only, before this block renders: the plan staged for
  // `revision`, if any, plays from this block.
  void release_staged_plan(std::uint64_t revision) noexcept { plans_->release_staged(revision); }
  // The newest offered revision a rendered block has used.
  [[nodiscard]] std::uint64_t applied_plan_revision() const noexcept;
  // Control only: whether offer_plan() has room now. False until the
  // callback has moved past enough earlier offers.
  [[nodiscard]] bool has_room() { return plans_->make_room(); }

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
  [[nodiscard]] const std::shared_ptr<native_dsp::synth::NativeSynthRuntime>&
  runtime() const noexcept {
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
  struct Plan;

public:
  class PreparedPlan {
  public:
    PreparedPlan(PreparedPlan&&) noexcept = default;
    PreparedPlan& operator=(PreparedPlan&&) noexcept = default;
    PreparedPlan(const PreparedPlan&) = delete;
    PreparedPlan& operator=(const PreparedPlan&) = delete;
    ~PreparedPlan();

  private:
    friend class NativeSynthPlayer;
    PreparedPlan(std::unique_ptr<Plan> plan, std::uint64_t revision) noexcept;
    std::unique_ptr<Plan> plan_;
    std::uint64_t revision_ = 0;
  };

private:
  // Everything a saved revision gives the player: swapped whole at a block.
  struct Plan {
    std::uint64_t revision = 0;
    // The project's tempo and meter: a tempo change is a new plan.
    std::shared_ptr<const timeline::TempoMap> tempo;
    timeline::BarGrid bars;
    std::shared_ptr<const engine::InstrumentEvents> events;
    std::shared_ptr<const engine::NoteSpans> spans;
    std::uint64_t last_safe_sample = 0;
    std::vector<std::shared_ptr<const automation_project::PreparedTargetAutomation>> evaluators;
    std::vector<std::uint32_t> parameters;
    std::vector<std::vector<double>> values;
    std::vector<native_dsp::ParameterLane> lanes;
    // Note effects (ADR-0059), per stage in chain order: values, switches
    // and the lanes of automated parameters (macro lanes), whose buffers
    // note_lane_values hold in note_evaluators' order.
    std::vector<std::vector<double>> note_values;
    std::vector<std::shared_ptr<const automation_project::PreparedTargetAutomation>>
        note_evaluators;
    std::vector<std::vector<double>> note_lane_values;
    std::vector<std::vector<native_dsp::ParameterLane>> note_lanes;
    std::vector<native_dsp::note_effects::StageControls> note_controls;
  };
  [[nodiscard]] foundation::Result<std::unique_ptr<Plan>>
  make_plan(const project::Snapshot& snapshot, const project::Track& track,
            const engine::InstrumentEvents& events,
            std::span<const automation_project::PreparedTargetAutomation> automation,
            std::uint64_t last_safe_automation_sample) const;

  project::TrackId track_;
  std::uint32_t sample_rate_ = 0;
  std::size_t max_frames_ = 0;
  std::shared_ptr<native_dsp::synth::NativeSynthRuntime> runtime_;
  std::vector<double> values_;
  // Boxed: the player moves, its atomics cannot.
  std::unique_ptr<native_dsp::synth::PlanMailbox<Plan>> plans_ =
      std::make_unique<native_dsp::synth::PlanMailbox<Plan>>();
  // Callback only: the events the last rendered block used, so the first
  // block of a new plan can hand its sounding notes over.
  const engine::InstrumentEvents* rendered_events_ = nullptr;
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
  std::shared_ptr<native_dsp::note_effects::NoteEffectChain> note_effects_;
  std::vector<engine::BlockMidiEvent> note_scratch_;
  // The track's notes just after each block, for the note effects (Comp's
  // pushes see the next chord coming).
  std::vector<native_dsp::note_effects::AheadEvent> note_ahead_;
  // Room for a chase of every pedal ahead of the block's own pedal events.
  std::vector<engine::BlockPedalEvent> block_pedals_;
  bool rendered_ = false;
  std::uint64_t rendered_generation_ = 0;
  std::uint64_t tail_frames_ = 0;
  std::optional<std::uint64_t> export_end_;
  // Boxed: the player moves, its atomics cannot.
  std::unique_ptr<std::atomic<bool>> retired_ = std::make_unique<std::atomic<bool>>(false);
};

} // namespace oscillate::native_inserts
