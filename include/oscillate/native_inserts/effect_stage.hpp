#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/plan_mailbox.hpp"
#include "oscillate/native_dsp/synth/tempo_sync.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// A built-in effect (ADR-0040) as an insert stage (ADR-0043): the generic
// NativeEffectRuntime behind the graph's processor interface, with its
// musical position and its slot's automation lanes. Live playback and offline
// export both process through this, so their timing and parameter semantics
// are identical.
namespace oscillate::native_inserts {

// The largest block a stage processes, and the last sample its automation
// may reach.
struct EffectStageLimits {
  std::size_t max_frames = 0;
  std::uint64_t last_safe_automation_sample = std::numeric_limits<std::uint64_t>::max();
};

class NativeEffectStage final : public engine::SidechainProcessorRuntime {
public:
  // Control only. Lanes targeting `slot` are taken from `automation` (none
  // without a slot); automated frames must not pass
  // last_safe_automation_sample.
  [[nodiscard]] static foundation::Result<std::unique_ptr<NativeEffectStage>>
  create(const project::Snapshot& snapshot, std::optional<project::InsertSlotId> slot,
         const project::NativeEffectPatch& patch,
         std::span<const automation_project::PreparedTargetAutomation> automation,
         EffectStageLimits limits);

  void process(std::span<engine::StereoFrame> stereo,
               engine::BlockContext context) noexcept override;
  void process_with_sidechain(std::span<engine::StereoFrame> stereo,
                              std::span<const engine::StereoFrame> sidechain,
                              engine::BlockContext context) noexcept override;
  // Mixer and send overlays never touch an insert's state.
  [[nodiscard]] bool control_overlay_revision_independent() const noexcept override { return true; }

  // Control only: for engines that read their keying tracks' notes
  // (EffectEngine::key_notes, ADR-0058), compile this project's note-ons of
  // the tracks keying this slot and offer them when they changed. Every
  // publish calls it, so a kept stage follows note edits without a rebuild.
  [[nodiscard]] foundation::Result<void> refresh_key_notes(const project::Snapshot& snapshot);
  // Control only: panel values, adopted at the next block (gliding).
  // Control only: a document edit (ADR-0058), offered before the values
  // with the same revision.
  [[nodiscard]] foundation::Result<void>
  offer_document(const project::NativeSynthDocument& document, std::uint64_t revision) {
    return runtime_->offer_document(document, revision);
  }
  [[nodiscard]] bool offer_values(std::span<const double> values, std::uint64_t revision) noexcept {
    return runtime_->offer_values(values, revision);
  }
  [[nodiscard]] bool offer_live_values(std::span<const double> values) noexcept {
    return runtime_->offer_live_values(values);
  }
  [[nodiscard]] std::uint64_t applied_values_revision() const noexcept {
    return runtime_->applied_revision();
  }
  // Control only. The slot's new lanes are built off callback and offered
  // without replacing the runtime or its sounding state.
  [[nodiscard]] foundation::Result<void>
  offer_automation(const project::Snapshot& snapshot,
                   std::span<const automation_project::PreparedTargetAutomation> automation,
                   std::uint64_t last_safe_automation_sample);
  // Control only: the same, held back for the live graph switch of the
  // snapshot's revision and adopted on the callback when that graph
  // activates, so a tempo or lane change lands on the switching block
  // (ADR-0098).
  [[nodiscard]] foundation::Result<void>
  stage_automation(const project::Snapshot& snapshot,
                   std::span<const automation_project::PreparedTargetAutomation> automation,
                   std::uint64_t last_safe_automation_sample);
  void graph_activated(std::uint64_t revision) noexcept override {
    automation_mailbox_->release_staged(revision);
  }
  [[nodiscard]] std::uint64_t applied_automation_revision() const noexcept {
    return automation_mailbox_->applied_revision();
  }
  [[nodiscard]] bool read_telemetry(std::vector<float>& out) {
    return runtime_->read_telemetry(out);
  }
  [[nodiscard]] const native_dsp::effects::EffectEngine& engine() const noexcept {
    return runtime_->engine();
  }
  [[nodiscard]] std::optional<project::InsertSlotId> slot() const noexcept { return slot_; }
  [[nodiscard]] std::uint32_t latency_frames() const noexcept { return runtime_->latency_frames(); }
  [[nodiscard]] double sample_rate() const noexcept { return static_cast<double>(sample_rate_); }
  // Blocks the stage refused (oversized, automation beyond its safe range or
  // a rejected kernel block); refused blocks pass through unprocessed.
  [[nodiscard]] std::uint64_t rejected_blocks() const noexcept {
    return rejected_.load(std::memory_order_relaxed);
  }
  // Control side, live monitoring only (never saved or exported): while on,
  // the stage outputs its sidechain key in place of its processed audio, so
  // the panel can audition what keys it. A stage with no key is unchanged.
  void set_key_listen(bool listen) noexcept {
    key_listen_.store(listen, std::memory_order_relaxed);
  }
  // Control side: the key's peak, linear, held and decaying over about 1 s.
  [[nodiscard]] float key_peak() const noexcept {
    return key_peak_.load(std::memory_order_relaxed);
  }

private:
  NativeEffectStage() = default;

  struct AutomationPlan {
    std::uint64_t revision = 0;
    // The project's tempo and meter: a tempo change is a new plan.
    std::shared_ptr<const timeline::TempoMap> tempo;
    native_dsp::synth::BarGrid bars;
    std::uint64_t last_safe_sample = 0;
    std::vector<std::shared_ptr<const automation_project::PreparedTargetAutomation>> evaluators;
    std::vector<std::uint32_t> parameters;
    std::vector<std::vector<double>> values;
    std::vector<native_dsp::synth::ParameterLane> lanes;
  };
  [[nodiscard]] foundation::Result<std::unique_ptr<AutomationPlan>>
  make_automation_plan(const project::Snapshot& snapshot,
                       std::span<const automation_project::PreparedTargetAutomation> automation,
                       std::uint64_t last_safe_automation_sample) const;
  void render(std::span<engine::StereoFrame> stereo, std::span<const engine::StereoFrame> sidechain,
              engine::BlockContext context) noexcept;

  std::optional<project::InsertSlotId> slot_;
  std::uint32_t sample_rate_ = 0;
  std::size_t max_frames_ = 0;
  std::unique_ptr<native_dsp::effects::NativeEffectRuntime> runtime_;
  std::unique_ptr<native_dsp::synth::PlanMailbox<AutomationPlan>> automation_mailbox_ =
      std::make_unique<native_dsp::synth::PlanMailbox<AutomationPlan>>();
  std::atomic<std::uint64_t> rejected_{0};
  std::atomic<bool> key_listen_{false};
  std::atomic<float> key_peak_{0.0F};
  float key_hold_ = 0.0F; // callback-owned
};

} // namespace oscillate::native_inserts
