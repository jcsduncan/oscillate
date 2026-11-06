#include "oscillate/engine/routed_node_meters.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace oscillate::engine {

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

RoutedNodeMeters::RoutedNodeMeters(std::size_t nodes)
    : slots_(std::make_unique<Slot[]>(nodes)), size_(nodes) {}

void RoutedNodeMeters::clear_window(Slot& slot) noexcept {
  slot.count = 0;
  slot.left_square_sum = 0.0;
  slot.right_square_sum = 0.0;
  slot.left_peak = 0.0F;
  slot.right_peak = 0.0F;
  slot.clipped = false;
}

void RoutedNodeMeters::publish(Slot& slot, const RoutedNodeMeterReading& reading) noexcept {
  slot.sequence.fetch_add(1);
  slot.left_peak_bits.store(std::bit_cast<std::uint32_t>(reading.left_peak));
  slot.right_peak_bits.store(std::bit_cast<std::uint32_t>(reading.right_peak));
  slot.left_rms_bits.store(std::bit_cast<std::uint32_t>(reading.left_rms));
  slot.right_rms_bits.store(std::bit_cast<std::uint32_t>(reading.right_rms));
  slot.end_sample.store(reading.end_sample);
  slot.published_generation.store(reading.transport_generation);
  slot.published_clipped.store(reading.clipped);
  slot.sequence.fetch_add(1);
}

void RoutedNodeMeters::observe(std::size_t node, std::span<const StereoFrame> frames,
                               BlockContext context) noexcept {
  if (node >= size_ || context.transport_generation == 0 ||
      frames.size() > std::numeric_limits<std::uint64_t>::max() - context.project_sample_position)
    return;
  auto& slot = slots_[node];
  if (slot.generation != context.transport_generation ||
      slot.expected_sample != context.project_sample_position) {
    // A planned loop wrap continues the window; anything else starts afresh
    // so a pre-seek level is not shown against the new position.
    const bool continuous_wrap = context.planned_loop_wrap && slot.generation != 0 &&
                                 context.transport_generation == slot.generation + 1;
    if (!continuous_wrap) {
      clear_window(slot);
      publish(slot, {.end_sample = context.project_sample_position,
                     .transport_generation = context.transport_generation});
    }
    slot.generation = context.transport_generation;
    slot.expected_sample = context.project_sample_position;
  }
  for (const auto& frame : frames) {
    const float left_abs = std::abs(frame.left);
    const float right_abs = std::abs(frame.right);
    slot.left_peak = std::max(slot.left_peak, left_abs);
    slot.right_peak = std::max(slot.right_peak, right_abs);
    slot.left_square_sum += static_cast<double>(frame.left) * frame.left;
    slot.right_square_sum += static_cast<double>(frame.right) * frame.right;
    slot.clipped = slot.clipped || left_abs >= 1.0F || right_abs >= 1.0F;
  }
  slot.count += frames.size();
  slot.expected_sample += frames.size();
  if (slot.count < window_frames)
    return;
  const auto count = static_cast<double>(slot.count);
  publish(slot, {.left_peak = slot.left_peak,
                 .right_peak = slot.right_peak,
                 .left_rms = static_cast<float>(std::sqrt(slot.left_square_sum / count)),
                 .right_rms = static_cast<float>(std::sqrt(slot.right_square_sum / count)),
                 .end_sample = slot.expected_sample,
                 .transport_generation = slot.generation,
                 .clipped = slot.clipped});
  clear_window(slot);
}

std::optional<RoutedNodeMeterReading> RoutedNodeMeters::latest(std::size_t node) const noexcept {
  if (node >= size_)
    return std::nullopt;
  const auto& slot = slots_[node];
  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto before = slot.sequence.load();
    if (before == 0 || (before & 1U) != 0)
      continue;
    RoutedNodeMeterReading reading{.left_peak = std::bit_cast<float>(slot.left_peak_bits.load()),
                                   .right_peak = std::bit_cast<float>(slot.right_peak_bits.load()),
                                   .left_rms = std::bit_cast<float>(slot.left_rms_bits.load()),
                                   .right_rms = std::bit_cast<float>(slot.right_rms_bits.load()),
                                   .end_sample = slot.end_sample.load(),
                                   .transport_generation = slot.published_generation.load(),
                                   .clipped = slot.published_clipped.load()};
    if (before == slot.sequence.load())
      return reading;
  }
  return std::nullopt;
}

} // namespace oscillate::engine
