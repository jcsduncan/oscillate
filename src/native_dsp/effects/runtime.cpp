#include "oscillate/native_dsp/effects/effect.hpp"

#include "oscillate/engine/denormals.hpp"
#include "oscillate/engine/midi_events.hpp"
#include "oscillate/foundation/trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace oscillate::native_dsp::effects {

using foundation::ErrorCode;
using foundation::Result;

Result<std::unique_ptr<NativeEffectRuntime>>
NativeEffectRuntime::create(const EffectEngine& engine, std::span<const double> values,
                            double sample_rate) {
  return create(engine, values, project::NativeSynthDocument{}, sample_rate);
}

Result<std::unique_ptr<NativeEffectRuntime>>
NativeEffectRuntime::create(const EffectEngine& engine, std::span<const double> values,
                            const project::NativeSynthDocument& document, double sample_rate) {
  using R = Result<std::unique_ptr<NativeEffectRuntime>>;
  if (engine.descriptor == nullptr || engine.create_kernel == nullptr ||
      engine.tail_frames == nullptr || engine.latency_frames == nullptr ||
      !(sample_rate >= 8'000.0 && sample_rate <= 384'000.0)) {
    return R::failure({ErrorCode::invalid_argument, "incomplete effect engine or bad rate"});
  }
  if (!synth::ParameterControl::valid_values(*engine.descriptor, values)) {
    return R::failure({ErrorCode::invalid_argument, "effect values do not match the engine"});
  }
  std::shared_ptr<const DocumentState> state;
  if (engine.prepare_document != nullptr) {
    auto prepared = engine.prepare_document(document, sample_rate, nullptr);
    if (!prepared) {
      return R::failure(prepared.error());
    }
    state = std::move(prepared).value();
  } else if (!document.empty()) {
    return R::failure({ErrorCode::invalid_argument,
                       std::string(engine.descriptor->name) + " has no structured state"});
  }
  auto kernel = engine.create_kernel(values, sample_rate);
  if (!kernel) {
    return R::failure({ErrorCode::internal, "effect engine did not create a kernel"});
  }
  if (state) {
    kernel->adopt_document(*state);
  }
  auto runtime = std::unique_ptr<NativeEffectRuntime>(
      new NativeEffectRuntime(engine, std::move(kernel), values, sample_rate));
  runtime->document_ = document;
  if (state) {
    runtime->documents_.push_back(std::make_unique<DocumentOffer>(0, std::move(state)));
  }
  return R::success(std::move(runtime));
}

Result<void> NativeEffectRuntime::offer_document(const project::NativeSynthDocument& document,
                                                 std::uint64_t revision) {
  if (engine_.prepare_document == nullptr) {
    if (!document.empty()) {
      return Result<void>::failure(
          {ErrorCode::invalid_argument,
           std::string(engine_.descriptor->name) + " has no structured state"});
    }
    return Result<void>::success();
  }
  if (!documents_.empty() && revision < documents_.back()->revision) {
    return Result<void>::failure({ErrorCode::conflict, "effect document revision went backwards"});
  }
  auto prepared = engine_.prepare_document(
      document, sample_rate_, documents_.empty() ? nullptr : documents_.back()->state.get());
  if (!prepared) {
    return Result<void>::failure(prepared.error());
  }
  // Retire offers the callback has moved past; keep the one it reads.
  const auto applied = document_mailbox_->applied.load(std::memory_order_acquire);
  std::erase_if(documents_, [applied](const auto& offer) { return offer->revision < applied; });
  documents_.push_back(std::make_unique<DocumentOffer>(revision, std::move(prepared).value()));
  document_mailbox_->offered.store(documents_.back().get(), std::memory_order_release);
  document_ = document;
  return Result<void>::success();
}

bool NativeEffectRuntime::offer_key_notes(std::vector<KeyNote> notes) {
  if (!key_notes_.empty() && *key_notes_.back() == notes)
    return false;
  // The callback can only be reading the applied list or about to adopt the
  // newest; every other list is retired.
  const auto* applied = key_notes_mailbox_->applied.load(std::memory_order_acquire);
  const auto* newest = key_notes_.empty() ? nullptr : key_notes_.back().get();
  std::erase_if(key_notes_,
                [&](const auto& list) { return list.get() != applied && list.get() != newest; });
  key_notes_.push_back(std::make_unique<const std::vector<KeyNote>>(std::move(notes)));
  key_notes_mailbox_->offered.store(key_notes_.back().get(), std::memory_order_release);
  return true;
}

void NativeEffectRuntime::adopt_key_notes() noexcept {
  const auto* offer = key_notes_mailbox_->offered.exchange(nullptr, std::memory_order_acq_rel);
  if (offer == nullptr)
    return;
  kernel_->adopt_key_notes(*offer);
  key_notes_mailbox_->applied.store(offer, std::memory_order_release);
}

void NativeEffectRuntime::adopt_document() noexcept {
  const auto* offer = document_mailbox_->offered.exchange(nullptr, std::memory_order_acq_rel);
  if (offer == nullptr) {
    return;
  }
  kernel_->adopt_document(*offer->state);
  document_mailbox_->applied.store(offer->revision, std::memory_order_release);
}

NativeEffectRuntime::NativeEffectRuntime(const EffectEngine& engine,
                                         std::unique_ptr<EffectKernel> kernel,
                                         std::span<const double> values, double sample_rate)
    : engine_(engine), kernel_(std::move(kernel)),
      control_(*engine.descriptor, values, sample_rate), sample_rate_(sample_rate),
      latency_(engine.latency_frames(values, sample_rate)),
      trace_instance_(foundation::trace::next_instance()) {}

EffectProcessStatus NativeEffectRuntime::process(std::span<engine::StereoFrame> audio,
                                                 std::span<const engine::StereoFrame> sidechain,
                                                 std::uint64_t project_frame,
                                                 std::uint64_t transport_generation,
                                                 const SynthTransport& transport,
                                                 std::span<const ParameterLane> lanes) noexcept {
  // The engine's display name is a string literal, so it names the span;
  // the instance number tells two of the same engine apart.
  OSCILLATE_TRACE_SPAN(engine_.descriptor->name.data(), trace_instance_);
  const engine::DenormalsFlushed flushed;
  if (audio.empty() || audio.size() > engine::max_midi_block_frames) {
    return EffectProcessStatus::invalid_block;
  }
  if (!sidechain.empty() && sidechain.size() != audio.size()) {
    return EffectProcessStatus::invalid_sidechain;
  }
  if (!control_.valid_lanes(lanes, audio.size())) {
    return EffectProcessStatus::invalid_lanes;
  }
  adopt_document();
  adopt_key_notes();
  control_.adopt();
  bool force_tick = !started_;
  if (started_ && transport_generation != generation_) {
    kernel_->reset();
    force_tick = true;
  }
  started_ = true;
  generation_ = transport_generation;

  std::size_t frame = 0;
  while (frame < audio.size()) {
    const std::uint64_t absolute = project_frame + frame;
    const auto phase = static_cast<std::uint32_t>(absolute % control_interval_frames);
    const std::uint32_t to_tick = control_interval_frames - phase;
    if (phase == 0 || force_tick) {
      SynthTransport at_tick = transport;
      const double beats = transport.beats_per_frame * static_cast<double>(frame);
      at_tick.beat_at_block_start += beats;
      at_tick.bar_at_block_start += beats / transport.beats_per_bar;
      kernel_->control(control_.tick(lanes, frame), to_tick, at_tick);
      force_tick = false;
    }
    const std::size_t end = std::min(audio.size(), frame + to_tick);
    kernel_->locate(absolute);
    kernel_->process(audio.subspan(frame, end - frame),
                     sidechain.empty() ? sidechain : sidechain.subspan(frame, end - frame));
    frame = end;
  }
  telemetry_->publish([this](std::span<float> out) { return kernel_->telemetry(out); });
  return EffectProcessStatus::rendered;
}

} // namespace oscillate::native_dsp::effects
