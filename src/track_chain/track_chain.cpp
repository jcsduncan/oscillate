#include "oscillate/track_chain/track_chain.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

namespace oscillate::track_chain {
namespace {
using foundation::ErrorCode;

[[nodiscard]] bool is_vst3(const SavedStage& stage) noexcept {
  return std::holds_alternative<project::Vst3PluginInstance>(stage.processor);
}

[[nodiscard]] std::optional<std::uint64_t> add_tail(std::optional<std::uint64_t> total,
                                                    std::optional<std::uint64_t> stage) noexcept {
  if (!total || !stage || *stage > std::numeric_limits<std::uint64_t>::max() - *total)
    return std::nullopt;
  return *total + *stage;
}

[[nodiscard]] foundation::Result<Stage> prepare_stage(const SavedStage& saved,
                                                      const ChainContext& context) {
  using StageResult = foundation::Result<Stage>;
  if (is_vst3(saved)) {
    if (!context.vst3)
      return StageResult::failure(
          {ErrorCode::unavailable, "No verified VST3 host is configured for this chain"});
    auto stage = context.vst3(saved);
    if (stage && (!stage.value().runtime || !(stage.value().saved == saved)))
      return StageResult::failure(
          {ErrorCode::corrupt_data, "VST3 host returned an incomplete stage"});
    return stage;
  }
  if (!saved.slot)
    return StageResult::failure(
        {ErrorCode::unsupported, "A built-in instrument renders as a source, not a chain stage"});
  auto prepared = native_inserts::prepare_native_stage(
      project::EffectInsert{*saved.slot, true, saved.processor}, context.native);
  if (!prepared)
    return StageResult::failure(prepared.error());
  auto& native = prepared.value();
  return StageResult::success({saved, std::move(native.runtime), native.latency_frames,
                               native.export_tail_frames, native.transition_tail_frames,
                               native.sidechain_capable});
}
} // namespace

std::vector<SavedStage> saved_chain(std::span<const project::EffectInsert> effects) {
  std::vector<SavedStage> result;
  result.reserve(effects.size());
  for (const auto& effect : effects) {
    if (effect.enabled)
      result.push_back({effect.id, effect.processor});
  }
  return result;
}

std::vector<SavedStage> saved_chain(const project::Track& track) {
  auto effects = saved_chain(track.effects);
  if (const auto* source = std::get_if<project::Vst3PluginInstance>(&track.instrument_source))
    effects.insert(effects.begin(), SavedStage{std::nullopt, *source});
  return effects;
}

// ---- ChainRuntime -----------------------------------------------------------

void ChainRuntime::Boundary::record(std::span<const engine::StereoFrame> block) noexcept {
  for (const auto frame : block) {
    frames[cursor] = frame;
    cursor = cursor + 1 == frames.size() ? 0 : cursor + 1;
    recorded = std::min(recorded + 1, frames.size());
  }
}

void ChainRuntime::Boundary::copy_newest(std::span<engine::StereoFrame> out) const noexcept {
  const auto size = frames.size();
  for (std::size_t index = 0; index < out.size(); ++index)
    out[index] = frames[(cursor + size - out.size() + index) % size];
}

ChainRuntime::ChainRuntime(std::vector<std::shared_ptr<engine::ProcessorRuntime>> stages,
                           std::vector<std::uint32_t> latencies, std::vector<bool> replayable,
                           std::optional<std::size_t> detector)
    : stages_(std::move(stages)), latencies_(std::move(latencies)),
      replayable_(std::move(replayable)), detector_(detector), boundaries_(stages_.size() + 1) {
  if (latencies_.size() != stages_.size() || replayable_.size() != stages_.size())
    throw std::invalid_argument("A chain needs one latency and one replay mark per stage");
  if (detector_ &&
      (*detector_ >= stages_.size() ||
       dynamic_cast<engine::SidechainProcessorRuntime*>(stages_[*detector_].get()) == nullptr))
    throw std::invalid_argument("A chain's detector must name a stage that takes a sidechain");
  for (auto& boundary : boundaries_)
    boundary.frames.resize(boundary_history_frames);
}

void ChainRuntime::process(std::span<engine::StereoFrame> stereo,
                           engine::BlockContext context) noexcept {
  process_from(0, stereo, {}, context);
}

void ChainRuntime::process_with_sidechain(std::span<engine::StereoFrame> stereo,
                                          std::span<const engine::StereoFrame> detector,
                                          engine::BlockContext context) noexcept {
  process_from(0, stereo, detector, context);
}

void ChainRuntime::process_from(std::size_t first, std::span<engine::StereoFrame> stereo,
                                std::span<const engine::StereoFrame> detector,
                                engine::BlockContext context) noexcept {
  for (std::size_t index = first; index < stages_.size(); ++index) {
    boundaries_[index].record(stereo);
    if (detector_ == index && !detector.empty())
      static_cast<engine::SidechainProcessorRuntime&>(*stages_[index])
          .process_with_sidechain(stereo, detector, context);
    else
      stages_[index]->process(stereo, context);
  }
  boundaries_.back().record(stereo);
}

bool ChainRuntime::contains(const engine::ProcessorRuntime* stage) const noexcept {
  return stage == this || std::ranges::any_of(stages_, [stage](const auto& owned) {
           return owned->contains(stage);
         });
}

void ChainRuntime::graph_activated(std::uint64_t revision) noexcept {
  for (const auto& stage : stages_)
    stage->graph_activated(revision);
}

void ChainRuntime::graph_deactivated(std::uint64_t revision) noexcept {
  for (const auto& stage : stages_)
    stage->graph_deactivated(revision);
}

bool ChainRuntime::control_overlay_revision_independent() const noexcept {
  return std::ranges::all_of(
      stages_, [](const auto& stage) { return stage->control_overlay_revision_independent(); });
}

void ChainRuntime::control_overlay_rebound(std::uint64_t from, std::uint64_t to) noexcept {
  for (const auto& stage : stages_)
    stage->control_overlay_rebound(from, to);
}

bool ChainRuntime::continues_latency_of(const engine::ProcessorRuntime* prior) const noexcept {
  if (prior == nullptr)
    return std::ranges::all_of(replayable_, std::identity{});
  const auto* old = dynamic_cast<const ChainRuntime*>(prior);
  return old != nullptr && changed_stage(*old).has_value();
}

std::size_t ChainRuntime::continue_latency_of(engine::ProcessorRuntime* prior,
                                              std::span<const engine::StereoFrame> input_older,
                                              std::span<const engine::StereoFrame> input_newer,
                                              engine::BlockContext boundary,
                                              std::span<engine::StereoFrame> flushed) noexcept {
  auto* old = dynamic_cast<ChainRuntime*>(prior);
  if (old == nullptr) {
    // Every stage is new: replay the path's recent input through the chain
    // so its lookahead and envelopes hold what they would have.
    if (prior == nullptr && std::ranges::all_of(replayable_, std::identity{}))
      replay(input_older, input_newer, boundary);
    return 0;
  }
  // Each kept stage carries its input history; each run of fresh stages is
  // primed from the input its first stage would have heard.
  const auto kept_at = [&](std::size_t index) -> std::optional<std::size_t> {
    const auto found = std::ranges::find(old->stages_, stages_[index]);
    if (found == old->stages_.end())
      return std::nullopt;
    return static_cast<std::size_t>(found - old->stages_.begin());
  };
  boundaries_.back() = old->boundaries_.back();
  for (std::size_t index = 0; index < stages_.size();) {
    if (const auto kept = kept_at(index)) {
      boundaries_[index] = old->boundaries_[*kept];
      ++index;
      continue;
    }
    const auto first = index;
    while (index < stages_.size() && !kept_at(index))
      ++index;
    // A fresh run starts the chain or follows a kept stage.
    std::size_t source = 0;
    if (first != 0) {
      const auto before = kept_at(first - 1);
      source = before ? *before + 1 : 0;
    }
    // A stage that cannot take earlier positions is not primed: it starts
    // from the live input, and only replayable runs get history.
    if (std::all_of(replayable_.begin() + static_cast<std::ptrdiff_t>(first),
                    replayable_.begin() + static_cast<std::ptrdiff_t>(index),
                    [](bool replays) { return replays; }))
      prime(first, index, old->boundaries_[source], boundary);
  }
  // A removed lookahead stage still held its last `latency` inputs: they
  // continue through the stages after it, as the frames the path emits first.
  const auto change = changed_stage(*old);
  if (!change || change->kind != Change::removed)
    return 0;
  const auto index = change->index;
  const auto& held = old->boundaries_[index];
  // After a loop wrap to a low position, history cannot reach before frame 0.
  const auto count = std::min<std::size_t>(
      {old->latencies_[index], held.recorded, flushed.size(),
       static_cast<std::size_t>(std::min<std::uint64_t>(boundary.project_sample_position,
                                                        std::numeric_limits<std::size_t>::max()))});
  held.copy_newest(flushed.first(count));
  for (std::size_t first = 0; first < count; first += chunk_frames) {
    auto chunk = flushed.subspan(first, std::min(chunk_frames, count - first));
    process_from(index, chunk, {},
                 {boundary.project_sample_position - count + first, boundary.transport_generation,
                  boundary.processing_generation});
  }
  return count;
}

std::uint32_t ChainRuntime::releasable_latency() const noexcept {
  std::uint64_t total = 0;
  for (const auto latency : latencies_)
    total += latency;
  return total <= boundary_history_frames ? static_cast<std::uint32_t>(total) : 0;
}

std::size_t ChainRuntime::release_latency(std::span<engine::StereoFrame> flushed) noexcept {
  const auto& input = boundaries_.front();
  const auto count = std::min<std::size_t>({releasable_latency(), input.recorded, flushed.size()});
  input.copy_newest(flushed.first(count));
  return count;
}

std::size_t ChainRuntime::prime_frames(std::size_t first, std::size_t last) const noexcept {
  std::uint64_t latency = 0;
  for (std::size_t index = first; index < last; ++index)
    latency += latencies_[index];
  return static_cast<std::size_t>(
      std::min<std::uint64_t>(latency + prime_margin_frames, boundary_history_frames));
}

void ChainRuntime::replay(std::span<const engine::StereoFrame> older,
                          std::span<const engine::StereoFrame> newer,
                          engine::BlockContext boundary) noexcept {
  const auto total =
      std::min({older.size() + newer.size(), prime_frames(0, stages_.size()),
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    boundary.project_sample_position, std::numeric_limits<std::size_t>::max()))});
  const auto skip = older.size() + newer.size() - total;
  std::array<engine::StereoFrame, chunk_frames> chunk{};
  for (std::size_t done = 0; done < total; done += chunk_frames) {
    const auto length = std::min(chunk_frames, total - done);
    for (std::size_t index = 0; index < length; ++index) {
      const auto at = skip + done + index;
      chunk[index] = at < older.size() ? older[at] : newer[at - older.size()];
    }
    process_from(0, std::span(chunk).first(length), {},
                 {boundary.project_sample_position - total + done, boundary.transport_generation,
                  boundary.processing_generation});
  }
}

void ChainRuntime::prime(std::size_t first, std::size_t last, const Boundary& input,
                         engine::BlockContext boundary) noexcept {
  std::array<engine::StereoFrame, chunk_frames> chunk{};
  const auto size = input.frames.size();
  const auto end = boundary.project_sample_position;
  const auto count = std::min({input.recorded, prime_frames(first, last),
                               static_cast<std::size_t>(std::min<std::uint64_t>(
                                   end, std::numeric_limits<std::size_t>::max()))});
  for (std::size_t done = 0; done < count; done += chunk_frames) {
    const auto length = std::min(chunk_frames, count - done);
    for (std::size_t frame = 0; frame < length; ++frame)
      chunk[frame] = input.frames[(input.cursor + size - count + done + frame) % size];
    const auto block = std::span(chunk).first(length);
    for (std::size_t index = first; index < last; ++index) {
      boundaries_[index].record(block);
      stages_[index]->process(block, {end - count + done, boundary.transport_generation,
                                      boundary.processing_generation});
    }
  }
}

std::optional<ChainRuntime::Change>
ChainRuntime::changed_stage(const ChainRuntime& old) const noexcept {
  // One stage added, removed or replaced, all others the same running
  // stages in the same order.
  const auto& longer = stages_.size() >= old.stages_.size() ? *this : old;
  const auto& shorter = stages_.size() >= old.stages_.size() ? old : *this;
  if (longer.stages_.size() - shorter.stages_.size() > 1)
    return std::nullopt;
  std::size_t index = 0;
  while (index < shorter.stages_.size() && shorter.stages_[index] == longer.stages_[index])
    ++index;
  const auto rest = static_cast<std::ptrdiff_t>(index);
  if (stages_.size() == old.stages_.size()) {
    if (index == stages_.size() ||
        !std::equal(stages_.begin() + rest + 1, stages_.end(), old.stages_.begin() + rest + 1))
      return std::nullopt;
    // A replacement keeps exact time when it is fresh and holds no less
    // than its predecessor: it re-emits, never skips, the path's samples.
    if (!replayable_[index] || latencies_[index] < old.latencies_[index] ||
        latencies_[index] > boundary_history_frames)
      return std::nullopt;
    return Change{index, Change::replaced};
  }
  if (!std::equal(shorter.stages_.begin() + rest, shorter.stages_.end(),
                  longer.stages_.begin() + rest + 1))
    return std::nullopt;
  const bool added = stages_.size() > old.stages_.size();
  const auto latency = longer.latencies_[index];
  if (latency == 0 || latency > boundary_history_frames)
    return std::nullopt;
  // An added stage is primed with earlier positions; a removed one's held
  // samples replay through the stages after it. Only fresh or built-in
  // stages accept earlier positions.
  const auto from = replayable_.begin() + rest;
  if (!std::all_of(from, added ? from + 1 : replayable_.end(), std::identity{}))
    return std::nullopt;
  return Change{index, added ? Change::added : Change::removed};
}

// ---- Preparation and switching ----------------------------------------------

std::uint32_t PreparedChain::latency_frames() const noexcept {
  std::uint32_t total = 0;
  for (const auto& stage : stages)
    total += stage.latency_frames;
  return total;
}

std::optional<std::uint64_t> PreparedChain::export_tail_frames() const noexcept {
  std::optional<std::uint64_t> total = 0;
  for (const auto& stage : stages)
    total = add_tail(total, stage.export_tail_frames);
  return total;
}

foundation::Result<PreparedChain> prepare_chain(std::span<const SavedStage> saved,
                                                const ChainContext& context,
                                                const PreparedChain* prior) {
  using ChainResult = foundation::Result<PreparedChain>;
  if (saved.size() > routing::ProjectRouteIdRegistry::max_stages_per_path)
    return ChainResult::failure({ErrorCode::unsupported, "Chain exceeds its stage limit"});
  try {
    PreparedChain chain;
    chain.stages.reserve(saved.size());
    const auto prior_stages =
        prior != nullptr ? std::span<const Stage>(prior->stages) : std::span<const Stage>{};
    std::vector<bool> taken(prior_stages.size(), false);
    std::vector<bool> fresh;
    fresh.reserve(saved.size());
    std::uint64_t latency = 0;
    std::optional<std::size_t> detector;
    for (const auto& stage : saved) {
      std::optional<std::size_t> kept;
      // A stage a sidechain now keys must have been prepared with its
      // detector, which a plugin activates only when keyed at preparation.
      const auto keyed = context.detector_slot && stage.slot == context.detector_slot;
      const bool reprepare = context.fresh_vst3_effects && stage.slot && is_vst3(stage);
      for (std::size_t index = 0; index < taken.size() && !reprepare; ++index) {
        if (!taken[index] && prior_stages[index].saved == stage &&
            (!keyed || prior_stages[index].detector)) {
          kept = index;
          break;
        }
      }
      if (kept) {
        taken[*kept] = true;
        chain.stages.push_back(prior_stages[*kept]);
        fresh.push_back(false);
      } else {
        auto prepared = prepare_stage(stage, context);
        if (!prepared)
          return ChainResult::failure(prepared.error());
        chain.stages.push_back(std::move(prepared).value());
        fresh.push_back(true);
      }
      const auto& added = chain.stages.back();
      latency += added.latency_frames;
      if (latency > std::numeric_limits<std::uint32_t>::max())
        return ChainResult::failure(
            {ErrorCode::resource_exhausted, "Chain latency exceeds 32-bit frame range"});
      if (context.detector_slot && added.saved.slot == context.detector_slot) {
        if (!added.detector ||
            dynamic_cast<engine::SidechainProcessorRuntime*>(added.runtime.get()) == nullptr)
          return ChainResult::failure(
              {ErrorCode::unsupported, "Sidechain target has no prepared detector stage"});
        detector = chain.stages.size() - 1;
      }
    }
    if (context.detector_slot && !detector)
      return ChainResult::failure(
          {ErrorCode::unsupported, "Sidechain target is not an enabled stage of this chain"});
    if (chain.stages.empty())
      return ChainResult::success(std::move(chain));
    std::vector<std::shared_ptr<engine::ProcessorRuntime>> runtimes;
    std::vector<std::uint32_t> latencies;
    std::vector<bool> replayable;
    runtimes.reserve(chain.stages.size());
    latencies.reserve(chain.stages.size());
    replayable.reserve(chain.stages.size());
    for (std::size_t index = 0; index < chain.stages.size(); ++index) {
      const auto& stage = chain.stages[index];
      runtimes.push_back(stage.runtime);
      latencies.push_back(stage.latency_frames);
      // A plugin that has run cannot take earlier positions again.
      replayable.push_back(!is_vst3(stage.saved) || fresh[index]);
    }
    chain.runtime = std::make_shared<ChainRuntime>(std::move(runtimes), std::move(latencies),
                                                   std::move(replayable), detector);
    return ChainResult::success(std::move(chain));
  } catch (const std::bad_alloc&) {
    return ChainResult::failure(
        {ErrorCode::resource_exhausted, "Chain preparation ran out of memory"});
  }
}

ChainSwitch switch_chain(const PreparedChain& prior, const PreparedChain& next,
                         std::uint64_t unbounded_ring_out_frames) {
  if (!prior.runtime)
    return {};
  // The removed stage that rings longest drains. A lookahead stage drains
  // too, unless an exact handover re-emits the samples it held.
  const bool handed_over = next.runtime && next.runtime->continues_latency_of(prior.runtime.get());
  std::uint64_t longest = 0;
  bool capped = false;
  std::shared_ptr<engine::ProcessorRuntime> drained;
  for (const auto& stage : prior.stages) {
    const bool kept = std::ranges::any_of(
        next.stages, [&](const Stage& successor) { return successor.runtime == stage.runtime; });
    if (kept || (stage.latency_frames != 0 && handed_over))
      continue;
    const auto tail = stage.ring_out_frames.value_or(unbounded_ring_out_frames);
    const auto ring = std::min(tail, engine::routed_tail_frame_cap);
    if (ring > longest) {
      longest = ring;
      capped = !stage.ring_out_frames || tail > ring;
      drained = stage.runtime;
    }
  }
  if (drained)
    return {engine::RoutedTailReplacement{prior.runtime, longest, std::move(drained), capped},
            nullptr};
  if (!next.runtime) {
    // Nothing runs any more: held lookahead returns to the dry path, or the
    // old chain retires through the mixer's minimal one-frame drain.
    if (prior.runtime->releasable_latency() != 0)
      return {};
    return {engine::RoutedTailReplacement{prior.runtime, 1}, nullptr};
  }
  return {std::nullopt, prior.runtime};
}

std::vector<routing::BoundStage> receipts(project::TrackId track, const PreparedChain& chain) {
  std::vector<routing::BoundStage> result;
  result.reserve(chain.stages.size());
  for (const auto& stage : chain.stages) {
    routing::BoundStage receipt{track, stage.saved.slot};
    if (const auto* plugin = std::get_if<project::Vst3PluginInstance>(&stage.saved.processor)) {
      receipt.processor = routing::StageProcessor::vst3;
      receipt.plugin = plugin->id;
    } else {
      receipt.processor = routing::StageProcessor::native_effect;
      receipt.generation = native_inserts::native_generation(stage.saved.processor);
    }
    receipt.latency_frames = stage.latency_frames;
    receipt.detector = stage.detector;
    result.push_back(receipt);
  }
  return result;
}

} // namespace oscillate::track_chain
