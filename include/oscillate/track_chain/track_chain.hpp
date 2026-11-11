#pragma once

#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/routed_mixer.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_inserts/native_stage.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/routing/project_route_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// One chain per track (ADR-0050): a track's VST3 instrument, when it has one,
// then each enabled effect in saved order, built-in or VST3. Live playback
// and export prepare every track and master chain here, and live playback
// switches chains here, stage by stage.
namespace oscillate::track_chain {

// One stage's saved identity: an effect slot, or (no slot) the track's VST3
// instrument.
struct SavedStage {
  std::optional<project::InsertSlotId> slot;
  project::EffectProcessor processor;
  bool operator==(const SavedStage&) const = default;
};

// The stages a saved track or master runs, in processing order.
[[nodiscard]] std::vector<SavedStage> saved_chain(const project::Track& track);
[[nodiscard]] std::vector<SavedStage> saved_chain(std::span<const project::EffectInsert> effects);

// One prepared stage.
struct Stage {
  SavedStage saved;
  std::shared_ptr<engine::ProcessorRuntime> runtime;
  // Constant processing delay the stage adds.
  std::uint32_t latency_frames = 0;
  // Output after the input stops, for export length; empty when unbounded.
  std::optional<std::uint64_t> export_tail_frames = 0;
  // Ring-out a removed stage needs; empty when the stage cannot bound it.
  std::optional<std::uint64_t> ring_out_frames = 0;
  // The stage reads a detector input, so a sidechain may key it.
  bool detector = false;
  // A VST3 stage's probe, true once a block failed to process. Built-in
  // stages have none: their guard reports a held non-finite frame.
  std::function<bool()> failed{};
};

// Prepares a VST3 stage, instrument or effect. The plugin host supplies it,
// so this library needs no VST3 code.
using Vst3Preparer = std::function<foundation::Result<Stage>(const SavedStage&)>;

struct ChainContext {
  native_inserts::StageContext native;
  Vst3Preparer vst3{};
  // The slot a sidechain keys, when one does.
  std::optional<project::InsertSlotId> detector_slot{};
  // Prepare every VST3 effect afresh rather than carry it over: it read the
  // tempo when it was prepared, and the tempo changed (ADR-0101).
  bool fresh_vst3_effects = false;
};

// A chain's stages in processing order. It records each stage boundary's
// recent input, so a live switch that adds or removes a lookahead stage keeps
// time (ADR-0049).
class ChainRuntime final : public engine::SidechainProcessorRuntime {
public:
  // The most a live latency handover replays per stage boundary.
  static constexpr std::size_t boundary_history_frames = 8192;

  // `replayable` marks the stages that accept earlier positions (built-in
  // ones, and plugins that have not run yet), which a handover replays;
  // `detector` indexes the stage a sidechain keys, if any. Throws
  // std::invalid_argument unless there is one latency and one replay mark
  // per stage and the detector names a stage that takes a sidechain.
  ChainRuntime(std::vector<std::shared_ptr<engine::ProcessorRuntime>> stages,
               std::vector<std::uint32_t> latencies, std::vector<bool> replayable,
               std::optional<std::size_t> detector);

  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext context) noexcept override;
  void process_with_sidechain(std::span<engine::StereoFrame> stereo,
                              std::span<const engine::StereoFrame> detector,
                              engine::BlockContext context) noexcept override;

  [[nodiscard]] bool contains(const engine::ProcessorRuntime* stage) const noexcept override;
  // Each stage of the chain, one at a time (ADR-0127).
  [[nodiscard]] std::size_t stage_count() const noexcept override { return stages_.size(); }
  [[nodiscard]] bool renders_any_split() const noexcept override {
    return std::ranges::all_of(stages_,
                               [](const auto& stage) { return stage->renders_any_split(); });
  }
  void process_stage(std::size_t stage, std::span<engine::StereoFrame> stereo,
                     std::span<const engine::StereoFrame> detector,
                     engine::BlockContext context) noexcept override;
  void graph_activated(std::uint64_t revision) noexcept override;
  void graph_deactivated(std::uint64_t revision) noexcept override;
  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override;
  void control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept override;

  // Whether the handover keeps exact time: a fresh chain on a path that had
  // none, or one stage added, removed or replaced (see changed_stage).
  [[nodiscard]] bool
  continues_latency_of(const engine::ProcessorRuntime* prior) const noexcept override;
  std::size_t continue_latency_of(engine::ProcessorRuntime* prior,
                                  std::span<const engine::StereoFrame> input_older,
                                  std::span<const engine::StereoFrame> input_newer,
                                  engine::BlockContext boundary,
                                  std::span<engine::StereoFrame> flushed) noexcept override;
  // Removed whole, the chain hands back the input its lookahead still held.
  [[nodiscard]] std::uint32_t releasable_latency() const noexcept override;
  std::size_t release_latency(std::span<engine::StereoFrame> flushed) noexcept override;

  [[nodiscard]] const std::vector<std::shared_ptr<engine::ProcessorRuntime>>&
  stages() const noexcept {
    return stages_;
  }
  [[nodiscard]] std::optional<std::size_t> detector() const noexcept { return detector_; }

private:
  static constexpr std::size_t chunk_frames = 64;
  static constexpr std::size_t prime_margin_frames = 2048;

  // The recent input of one stage (or the chain's output), oldest first.
  struct Boundary {
    std::vector<engine::StereoFrame> frames;
    std::size_t cursor = 0;
    std::size_t recorded = 0;
    void record(std::span<const engine::StereoFrame> block) noexcept;
    void copy_newest(std::span<engine::StereoFrame> out) const noexcept;
  };

  void process_from(std::size_t first, std::span<engine::StereoFrame> stereo,
                    std::span<const engine::StereoFrame> detector,
                    engine::BlockContext context) noexcept;
  // Frames to replay into stages [first, last): their lookahead and a margin
  // for envelopes and tails, within the recorded history.
  [[nodiscard]] std::size_t prime_frames(std::size_t first, std::size_t last) const noexcept;
  void replay(std::span<const engine::StereoFrame> older,
              std::span<const engine::StereoFrame> newer, engine::BlockContext boundary) noexcept;
  // Feeds the fresh stages [first, last) the recent `input` their first
  // stage would have heard, ending just before `boundary`.
  void prime(std::size_t first, std::size_t last, const Boundary& input,
             engine::BlockContext boundary) noexcept;
  // The one stage this chain adds to, removes from or replaces in `old`, all
  // others being the same running stages in the same order, when that
  // keeps exact time: its index here (added, replaced) or in `old`
  // (removed).
  struct Change {
    std::size_t index;
    enum Kind : std::uint8_t { added, removed, replaced } kind;
  };
  [[nodiscard]] std::optional<Change> changed_stage(const ChainRuntime& old) const noexcept;

  std::vector<std::shared_ptr<engine::ProcessorRuntime>> stages_;
  std::vector<std::uint32_t> latencies_;
  std::vector<bool> replayable_;
  std::optional<std::size_t> detector_;
  std::vector<Boundary> boundaries_;
};

// A prepared chain: its runtime (empty when no stage runs) and its stages.
struct PreparedChain {
  std::shared_ptr<ChainRuntime> runtime;
  std::vector<Stage> stages;
  [[nodiscard]] std::uint32_t latency_frames() const noexcept;
  // Empty when a stage's tail is unbounded.
  [[nodiscard]] std::optional<std::uint64_t> export_tail_frames() const noexcept;
};

// Control side only. Prepares `saved`, carrying over each running stage of
// `prior` whose saved identity is unchanged, so its state continues; only new
// and changed stages are prepared.
[[nodiscard]] foundation::Result<PreparedChain> prepare_chain(std::span<const SavedStage> saved,
                                                              const ChainContext& context,
                                                              const PreparedChain* prior = nullptr);

// How a chain's predecessor retires at a live switch (ADR-0050). A removed
// stage that rings drains on zero input beside the successor (the longest,
// when several go); a removed lookahead stage hands its held samples on
// through the mixer's latency handover; otherwise the kept stages pass to the
// successor by a state handoff.
struct ChainSwitch {
  std::optional<engine::RoutedTailReplacement> drain;
  std::shared_ptr<engine::ProcessorRuntime> handoff;
};
// `unbounded_ring_out_frames` is how long a removed stage that cannot bound
// its tail rings before the mixer fades it out.
[[nodiscard]] ChainSwitch switch_chain(const PreparedChain& prior, const PreparedChain& next,
                                       std::uint64_t unbounded_ring_out_frames);

// The route receipts for a track's prepared chain.
[[nodiscard]] std::vector<routing::BoundStage> receipts(project::TrackId track,
                                                        const PreparedChain& chain);

} // namespace oscillate::track_chain
