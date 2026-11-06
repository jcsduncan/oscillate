#include "oscillate/native_inserts/effect_stage.hpp"

#include "oscillate/engine/midi_events.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <variant>

namespace oscillate::native_inserts {
namespace {

using foundation::ErrorCode;
using foundation::Result;

// The note-ons of every track that keys `slot`, sorted and unique. Notes a
// clip boundary restarts are not new hits.
Result<std::vector<native_dsp::effects::KeyNote>> key_note_onsets(const project::Snapshot& snapshot,
                                                                  project::InsertSlotId slot) {
  using R = Result<std::vector<native_dsp::effects::KeyNote>>;
  std::vector<project::TrackId> keys;
  for (const auto& track : snapshot.tracks) {
    if (std::ranges::any_of(track.sends, [&](const project::SendRoute& send) {
          return send.purpose == project::SendPurpose::sidechain && send.target_insert == slot;
        }))
      keys.push_back(track.id);
  }
  std::vector<native_dsp::effects::KeyNote> onsets;
  if (keys.empty())
    return R::success(std::move(onsets));
  auto plan = engine::compile_midi_events(snapshot);
  if (!plan)
    return R::failure(plan.error());
  for (const auto& instrument : plan.value().instruments()) {
    if (std::ranges::find(keys, instrument.track()) == keys.end())
      continue;
    for (const auto& event : instrument.events()) {
      if (event.kind == engine::MidiEventKind::note_on && !event.boundary_crop)
        onsets.push_back({event.sample.value, event.key, event.velocity});
    }
  }
  std::ranges::sort(onsets);
  const auto [first, last] = std::ranges::unique(onsets);
  onsets.erase(first, last);
  return R::success(std::move(onsets));
}

} // namespace

Result<void> NativeEffectStage::refresh_key_notes(const project::Snapshot& snapshot) {
  if (!runtime_->engine().key_notes || !slot_)
    return Result<void>::success();
  auto onsets = key_note_onsets(snapshot, *slot_);
  if (!onsets)
    return Result<void>::failure(onsets.error());
  try {
    (void)runtime_->offer_key_notes(std::move(onsets).value());
  } catch (const std::bad_alloc&) {
    return Result<void>::failure(
        {ErrorCode::resource_exhausted, "Cannot prepare the effect's key notes"});
  }
  return Result<void>::success();
}

Result<std::unique_ptr<NativeEffectStage>>
NativeEffectStage::create(const project::Snapshot& snapshot,
                          std::optional<project::InsertSlotId> slot,
                          const project::NativeEffectPatch& patch,
                          std::span<const automation_project::PreparedTargetAutomation> automation,
                          std::size_t max_frames, std::uint64_t last_safe_automation_sample) {
  using R = Result<std::unique_ptr<NativeEffectStage>>;
  if (max_frames == 0 || max_frames > engine::max_midi_block_frames)
    return R::failure({ErrorCode::invalid_argument, "Effect stage block size is invalid"});
  auto resolved = resolve_native_effect(patch);
  if (!resolved)
    return R::failure(resolved.error());
  auto runtime = native_dsp::effects::NativeEffectRuntime::create(
      *resolved.value().engine, resolved.value().values, patch.document, snapshot.sample_rate);
  if (!runtime)
    return R::failure(runtime.error());
  try {
    std::unique_ptr<NativeEffectStage> stage(new NativeEffectStage());
    stage->slot_ = slot;
    stage->sample_rate_ = snapshot.sample_rate;
    stage->max_frames_ = max_frames;
    stage->runtime_ = std::move(runtime).value();
    stage->tempo_ = std::make_shared<const timeline::TempoMap>(snapshot.tempo);
    stage->bars_ = native_dsp::synth::BarGrid(snapshot.meter);
    if (auto notes = stage->refresh_key_notes(snapshot); !notes)
      return R::failure(notes.error());
    auto plan = stage->make_automation_plan(snapshot, automation, last_safe_automation_sample);
    if (!plan)
      return R::failure(plan.error());
    stage->automation_mailbox_->install(std::move(plan).value(), snapshot.revision);
    return R::success(std::move(stage));
  } catch (const std::bad_alloc&) {
    return R::failure({ErrorCode::resource_exhausted, "Cannot prepare a built-in effect stage"});
  }
}

Result<std::unique_ptr<NativeEffectStage::AutomationPlan>> NativeEffectStage::make_automation_plan(
    const project::Snapshot& snapshot,
    std::span<const automation_project::PreparedTargetAutomation> automation,
    std::uint64_t last_safe_automation_sample) const {
  using R = Result<std::unique_ptr<AutomationPlan>>;
  if (snapshot.sample_rate != sample_rate_)
    return R::failure({ErrorCode::conflict, "Effect automation does not match the running stage"});
  auto plan = std::make_unique<AutomationPlan>();
  plan->revision = snapshot.revision;
  plan->last_safe_sample = last_safe_automation_sample;
  for (const auto& prepared : automation) {
    const auto* target = std::get_if<project::NativeEffectParameterTarget>(&prepared.target());
    if (!target || !slot_ || target->slot != *slot_)
      continue;
    if (prepared.snapshot_revision() != snapshot.revision)
      return R::failure({ErrorCode::conflict, "Effect automation plan revision is stale"});
    auto index = native_effect_parameter_index(runtime_->engine(), target->parameter_id);
    if (!index)
      return R::failure(index.error());
    plan->evaluators.push_back(
        std::make_shared<const automation_project::PreparedTargetAutomation>(prepared));
    plan->parameters.push_back(static_cast<std::uint32_t>(index.value()));
    plan->values.emplace_back(max_frames_);
  }
  plan->lanes.resize(plan->evaluators.size());
  return R::success(std::move(plan));
}

Result<void> NativeEffectStage::offer_automation(
    const project::Snapshot& snapshot,
    std::span<const automation_project::PreparedTargetAutomation> automation,
    std::uint64_t last_safe_automation_sample) {
  if (automation_mailbox_->stale(snapshot.revision))
    return Result<void>::failure({ErrorCode::conflict, "Effect automation revision is stale"});
  if (!automation_mailbox_->make_room())
    return Result<void>::failure(
        {ErrorCode::resource_exhausted,
         "Live effect automation mailbox is full; retry after the callback"});
  try {
    auto plan = make_automation_plan(snapshot, automation, last_safe_automation_sample);
    if (!plan)
      return Result<void>::failure(plan.error());
    automation_mailbox_->offer(std::move(plan).value(), snapshot.revision);
    return Result<void>::success();
  } catch (const std::bad_alloc&) {
    return Result<void>::failure(
        {ErrorCode::resource_exhausted, "Cannot prepare live effect automation"});
  }
}

void NativeEffectStage::process(std::span<engine::StereoFrame> stereo,
                                engine::BlockContext context) noexcept {
  render(stereo, {}, context);
}

void NativeEffectStage::process_with_sidechain(std::span<engine::StereoFrame> stereo,
                                               std::span<const engine::StereoFrame> sidechain,
                                               engine::BlockContext context) noexcept {
  render(stereo, sidechain, context);
}

void NativeEffectStage::render(std::span<engine::StereoFrame> stereo,
                               std::span<const engine::StereoFrame> sidechain,
                               engine::BlockContext context) noexcept {
  if (stereo.empty())
    return;
  if (stereo.size() > max_frames_) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto first = context.project_sample_position;
  const auto frames = stereo.size();
  auto* plan = automation_mailbox_->current();
  std::span<const native_dsp::synth::ParameterLane> lanes;
  // Beyond the plan's safe range the panel values play on unautomated.
  if (plan && !plan->evaluators.empty() && first <= plan->last_safe_sample &&
      frames - 1 <= plan->last_safe_sample - first) {
    bool evaluated = true;
    for (std::size_t lane = 0; lane < plan->evaluators.size() && evaluated; ++lane) {
      const auto values = std::span(plan->values[lane]).first(frames);
      evaluated = plan->evaluators[lane]->evaluate_block(
          timeline::SamplePosition{static_cast<std::int64_t>(first)}, values);
      plan->lanes[lane] = {plan->parameters[lane], values};
    }
    if (evaluated)
      lanes = plan->lanes;
    else
      rejected_.fetch_add(1, std::memory_order_relaxed);
  }
  // Musical position for tempo-synced effects, at the block start.
  native_dsp::synth::SynthTransport transport;
  timeline::TickPosition tick{};
  double bpm = 0.0;
  const timeline::SamplePosition at{static_cast<std::int64_t>(first)};
  if (tempo_->sample_to_tick_rt(at, sample_rate_, tick) &&
      tempo_->bpm_at_sample_rt(at, sample_rate_, bpm) && bpm > 0.0) {
    transport.beat_at_block_start =
        static_cast<double>(tick.value) / static_cast<double>(timeline::ticks_per_quarter);
    transport.beats_per_frame = bpm / 60.0 / static_cast<double>(sample_rate_);
    bars_.locate(tick.value, transport.bar_at_block_start, transport.beats_per_bar);
  }
  const auto status =
      runtime_->process(stereo, sidechain, first, context.state_generation(), transport, lanes);
  if (status != native_dsp::effects::EffectProcessStatus::rendered) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (plan)
    automation_mailbox_->applied(plan->revision);
  // The key's held peak, falling 60 dB a second, for the panel's key meter.
  float peak = 0.0F;
  for (const auto& frame : sidechain)
    peak = std::max({peak, std::fabs(frame.left), std::fabs(frame.right)});
  const auto fall = std::pow(1e-3F, static_cast<float>(frames) / static_cast<float>(sample_rate_));
  key_hold_ = std::isfinite(peak) ? std::max(peak, key_hold_ * fall) : key_hold_ * fall;
  key_peak_.store(key_hold_, std::memory_order_relaxed);
  if (key_listen_.load(std::memory_order_relaxed) && sidechain.size() == stereo.size())
    std::ranges::copy(sidechain, stereo.begin());
}

} // namespace oscillate::native_inserts
