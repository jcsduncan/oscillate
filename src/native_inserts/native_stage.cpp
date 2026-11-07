#include "oscillate/native_inserts/native_stage.hpp"

#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/effect_stage.hpp"

#include <algorithm>
#include <new>
#include <type_traits>
#include <variant>

namespace oscillate::native_inserts {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using StageResult = foundation::Result<PreparedStage>;

[[nodiscard]] StageResult prepare_effect_stage(const project::NativeEffectPatch& saved,
                                               std::optional<project::InsertSlotId> slot,
                                               const StageContext& context) {
  if (context.snapshot == nullptr)
    return StageResult::failure(
        {ErrorCode::invalid_argument, "A built-in effect stage needs its project"});
  auto created = NativeEffectStage::create(*context.snapshot, slot, saved, context.automation,
                                           context.max_frames, context.last_safe_automation_sample);
  if (!created)
    return StageResult::failure(created.error());
  auto resolved = resolve_native_effect(saved);
  if (!resolved)
    return StageResult::failure(resolved.error());
  const auto& engine = *resolved.value().engine;
  const auto& values = resolved.value().values;
  const auto rate = static_cast<double>(context.snapshot->sample_rate);
  PreparedStage stage;
  stage.kind = StageKind::effect;
  stage.generation = saved.generation;
  stage.latency_frames = engine.latency_frames(values, rate);
  const auto tail = engine.tail_frames(values, rate);
  stage.export_tail_frames = tail;
  if (stage.latency_frames == 0) {
    stage.transition_tail_frames = std::max<std::uint64_t>(1, tail);
  } else {
    stage.transition_tail_frames.reset();
    stage.transition_rule = TransitionRule::none;
    stage.tail_error =
        Error{ErrorCode::unsupported, "Native effect has no zero-latency tail contract"};
  }
  stage.sidechain_capable = engine.sidechain;
  stage.runtime = std::shared_ptr<NativeEffectStage>(std::move(created).value());
  return StageResult::success(std::move(stage));
}

[[nodiscard]] StageResult prepare_stage(const project::EffectProcessor& processor,
                                        std::optional<project::InsertSlotId> slot,
                                        const StageContext& context) {
  try {
    return std::visit(
        [&](const auto& saved) -> StageResult {
          using T = std::decay_t<decltype(saved)>;
          if constexpr (std::is_same_v<T, project::NativeConvolution>) {
            if (!context.convolution)
              return StageResult::failure(
                  {ErrorCode::unsupported, "This build has no live convolution runtime"});
            return context.convolution(saved);
          } else if constexpr (std::is_same_v<T, project::NativeEffectPatch>) {
            return prepare_effect_stage(saved, slot, context);
          } else {
            static_assert(std::is_same_v<T, project::Vst3PluginInstance>);
            return StageResult::failure(
                {ErrorCode::unsupported, "A VST3 slot is not a native insert"});
          }
        },
        processor);
  } catch (const std::bad_alloc&) {
    return StageResult::failure(
        {ErrorCode::resource_exhausted, "Native insert preparation ran out of memory"});
  }
}

} // namespace

foundation::Result<PreparedStage> prepare_native_stage(const project::EffectInsert& slot,
                                                       const StageContext& context) {
  return prepare_stage(slot.processor, slot.id, context);
}

foundation::Result<PreparedStage> prepare_native_stage(const project::EffectProcessor& processor,
                                                       const StageContext& context) {
  return prepare_stage(processor, std::nullopt, context);
}

bool is_native(const project::EffectProcessor& processor) noexcept {
  return !std::holds_alternative<project::Vst3PluginInstance>(processor);
}

// NOLINTNEXTLINE(bugprone-exception-escape): a saved processor is never valueless.
std::uint64_t native_generation(const project::EffectProcessor& processor) noexcept {
  return std::visit(
      [](const auto& saved) -> std::uint64_t {
        if constexpr (std::is_same_v<std::decay_t<decltype(saved)>, project::Vst3PluginInstance>)
          return 0;
        else
          return saved.generation;
      },
      processor);
}

std::optional<std::uint64_t>
guarded_frames_after_quiesce(const engine::ProcessorRuntime& runtime) noexcept {
  // A built-in effect never holds output; a block it refused passed through
  // unprocessed, which an offline render must not publish.
  if (const auto* stage = dynamic_cast<const NativeEffectStage*>(&runtime))
    return stage->rejected_blocks();
  return std::nullopt;
}

} // namespace oscillate::native_inserts
