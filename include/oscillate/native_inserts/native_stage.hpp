#pragma once

#include "oscillate/automation_project/preparation.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>

// One native insert path (ADR-0043): every chain builder (live track and
// master, offline track and master, the VST3 export drivers) prepares native
// insert stages here and only here.
namespace oscillate::native_inserts {

enum class StageKind : std::uint8_t { convolution, effect };

// How a stage contributes to a master chain's replacement ring-out: the first
// stage that sets it, a stage that overrides it, or not at all.
enum class TransitionRule : std::uint8_t { fill_if_unset, override, none };

struct PreparedStage {
  std::shared_ptr<engine::ProcessorRuntime> runtime;
  StageKind kind = StageKind::effect;
  std::uint64_t generation = 0;
  // Constant processing delay the stage adds.
  std::uint32_t latency_frames = 0;
  // Output after the input stops, for export length. Empty when it cannot be
  // bounded (`tail_error` says why); latency is accounted separately.
  std::optional<std::uint64_t> export_tail_frames = 0;
  // Ring-out a replaced or removed stage needs before retirement. Empty when
  // the stage has no bounded zero-latency tail contract (`tail_error`).
  std::optional<std::uint64_t> transition_tail_frames = 1;
  TransitionRule transition_rule = TransitionRule::fill_if_unset;
  std::optional<foundation::Error> tail_error;
  // The stage accepts a detector input (a sidechain send may target it).
  bool sidechain_capable = false;
};

// Prepares a convolution stage; live and offline playback admit IRs
// differently (store, budget, headroom), so they supply it.
using ConvolutionPreparer =
    std::function<foundation::Result<PreparedStage>(const project::NativeConvolution&)>;

struct StageContext {
  std::uint32_t sample_rate = 0;
  ConvolutionPreparer convolution{};
  // Built-in effects (ADR-0040) need the project's tempo and meter for their
  // musical position, and take their slot's lanes from `automation`.
  const project::Snapshot* snapshot = nullptr;
  std::span<const automation_project::PreparedTargetAutomation> automation{};
  std::size_t max_frames = engine::max_midi_block_frames;
  std::uint64_t last_safe_automation_sample = std::numeric_limits<std::uint64_t>::max();
};

// Control side only. VST3 slots are not native and are refused.
[[nodiscard]] foundation::Result<PreparedStage>
prepare_native_stage(const project::EffectInsert& slot, const StageContext& context);
// As above, for a processor outside a saved slot (tails, previews): a
// built-in effect gets no automation.
[[nodiscard]] foundation::Result<PreparedStage>
prepare_native_stage(const project::EffectProcessor& processor, const StageContext& context);

// True for every processor this factory prepares.
[[nodiscard]] bool is_native(const project::EffectProcessor& processor) noexcept;

// The saved generation of a native processor (0 for VST3).
[[nodiscard]] std::uint64_t native_generation(const project::EffectProcessor& processor) noexcept;

// Frames a native stage still guards after a quiesce (a held fade or flush);
// empty for runtimes this library does not know.
[[nodiscard]] std::optional<std::uint64_t>
guarded_frames_after_quiesce(const engine::ProcessorRuntime& runtime) noexcept;

} // namespace oscillate::native_inserts
