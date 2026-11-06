#include "oscillate/native_dsp/synth/parameter_control.hpp"

#include "oscillate/foundation/trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth {

ParameterControl::ParameterControl(const EngineDescriptor& descriptor,
                                   std::span<const double> values, double sample_rate)
    : descriptor_(&descriptor), count_(values.size()), sample_rate_(sample_rate),
      target_(values.begin(), values.end()), current_(values.begin(), values.end()),
      glide_step_(values.size(), 0.0), effective_(values.begin(), values.end()) {}

bool ParameterControl::valid_values(const EngineDescriptor& descriptor,
                                    std::span<const double> values) noexcept {
  if (values.size() != descriptor.parameters.size()) {
    return false;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto& parameter = descriptor.parameters[i];
    if (!std::isfinite(values[i]) || values[i] < parameter.minimum ||
        values[i] > parameter.maximum ||
        (is_discrete(parameter) && values[i] != std::round(values[i]))) {
      return false;
    }
  }
  return true;
}

bool ParameterControl::offer(std::span<const double> values, std::uint64_t revision) noexcept {
  if (revision == 0 || revision <= mailbox_->offered || !push(values, revision))
    return false;
  mailbox_->offered = revision;
  return true;
}

bool ParameterControl::offer_live(std::span<const double> values) noexcept {
  return push(values, 0);
}

bool ParameterControl::push(std::span<const double> values, std::uint64_t revision) noexcept {
  if (!valid_values(*descriptor_, values))
    return false;
  const auto write = mailbox_->write.load(std::memory_order_relaxed);
  if (write - mailbox_->read.load(std::memory_order_acquire) >= Mailbox::capacity) {
    return false;
  }
  auto& entry = mailbox_->entries[write % Mailbox::capacity];
  std::copy(values.begin(), values.end(), entry.values.begin());
  entry.revision = revision;
  mailbox_->write.store(write + 1, std::memory_order_release);
  return true;
}

std::uint64_t ParameterControl::applied_revision() const noexcept {
  return mailbox_->applied.load(std::memory_order_acquire);
}

void ParameterControl::adopt() noexcept {
  const auto read = mailbox_->read.load(std::memory_order_relaxed);
  const auto write = mailbox_->write.load(std::memory_order_acquire);
  if (read == write) {
    return;
  }
  const auto& newest = mailbox_->entries[(write - 1) % Mailbox::capacity];
  const auto& parameters = descriptor_->parameters;
  const double tick_seconds = control_interval_frames / sample_rate_;
  for (std::size_t i = 0; i < count_; ++i) {
    const auto& parameter = parameters[i];
    target_[i] = newest.values[i];
    if (is_discrete(parameter) || parameter.glide_seconds <= 0.0) {
      current_[i] = target_[i];
      glide_step_[i] = 0.0;
      continue;
    }
    const double distance =
        std::fabs(to_normalised(parameter, target_[i]) - to_normalised(parameter, current_[i]));
    const double ticks = std::max(1.0, parameter.glide_seconds / tick_seconds);
    glide_step_[i] = distance / ticks;
  }
  // The newest saved revision among the consumed offers; live ones carry none.
  std::uint64_t revision = 0;
  for (auto index = read; index != write; ++index)
    revision = std::max(revision, mailbox_->entries[index % Mailbox::capacity].revision);
  mailbox_->read.store(write, std::memory_order_release);
  // An observed applied revision also guarantees the consumed slot is free.
  if (revision != 0)
    mailbox_->applied.store(revision, std::memory_order_release);
  // Where a committed edit, or a drag's newest value, reaches the audio.
  foundation::trace::instant(revision != 0 ? "edit.audible" : "edit.live_audible", revision);
}

bool ParameterControl::valid_lanes(std::span<const ParameterLane> lanes,
                                   std::size_t frames) const noexcept {
  const auto& parameters = descriptor_->parameters;
  for (const auto& lane : lanes) {
    if (lane.parameter >= count_ || lane.values.size() != frames) {
      return false;
    }
    const auto& parameter = parameters[lane.parameter];
    if (!parameter.automatable) {
      return false;
    }
    // Discrete lanes are stepped (ADR-0034): whole values only.
    const bool discrete = is_discrete(parameter);
    for (const double value : lane.values) {
      if (!std::isfinite(value) || value < parameter.minimum || value > parameter.maximum ||
          (discrete && value != std::round(value))) {
        return false;
      }
    }
  }
  return true;
}

std::span<const double> ParameterControl::tick(std::span<const ParameterLane> lanes,
                                               std::size_t block_offset) noexcept {
  const auto& parameters = descriptor_->parameters;
  for (std::size_t i = 0; i < count_; ++i) {
    if (glide_step_[i] > 0.0) {
      const auto& parameter = parameters[i];
      const double now = to_normalised(parameter, current_[i]);
      const double goal = to_normalised(parameter, target_[i]);
      const double moved =
          goal > now ? std::min(goal, now + glide_step_[i]) : std::max(goal, now - glide_step_[i]);
      if (moved == goal) {
        current_[i] = target_[i];
        glide_step_[i] = 0.0;
      } else {
        current_[i] = from_normalised(parameter, moved);
      }
    }
    effective_[i] = current_[i];
  }
  for (const auto& lane : lanes) {
    effective_[lane.parameter] = lane.values[block_offset];
  }
  return effective_;
}

bool TelemetryBuffer::read(std::vector<float>& out) {
  if ((middle_.load(std::memory_order_acquire) & fresh) != 0) {
    const auto previous = middle_.exchange(front_, std::memory_order_acq_rel);
    front_ = static_cast<std::uint8_t>(previous & 3U);
    any_ = true;
  }
  if (!any_) {
    return false;
  }
  const auto& slot = frames_[front_];
  out.assign(slot.values.begin(), slot.values.begin() + static_cast<std::ptrdiff_t>(slot.size));
  return true;
}

} // namespace oscillate::native_dsp::synth
