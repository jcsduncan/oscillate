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
  const auto index = slot.published.load(std::memory_order_relaxed);
  auto& window = slot.windows[index % history];
  window.sequence.fetch_add(1);
  window.left_peak_bits.store(std::bit_cast<std::uint32_t>(reading.left_peak));
  window.right_peak_bits.store(std::bit_cast<std::uint32_t>(reading.right_peak));
  window.left_rms_bits.store(std::bit_cast<std::uint32_t>(reading.left_rms));
  window.right_rms_bits.store(std::bit_cast<std::uint32_t>(reading.right_rms));
  window.end_sample.store(reading.end_sample);
  window.published_generation.store(reading.transport_generation);
  window.published_clipped.store(reading.clipped);
  window.sequence.fetch_add(1);
  slot.published.store(index + 1);
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

std::optional<RoutedNodeMeterReading> RoutedNodeMeters::read(const Window& window) noexcept {
  constexpr int attempts = 4;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const auto before = window.sequence.load();
    if (before == 0 || (before & 1U) != 0)
      continue;
    RoutedNodeMeterReading reading{.left_peak = std::bit_cast<float>(window.left_peak_bits.load()),
                                   .right_peak =
                                       std::bit_cast<float>(window.right_peak_bits.load()),
                                   .left_rms = std::bit_cast<float>(window.left_rms_bits.load()),
                                   .right_rms = std::bit_cast<float>(window.right_rms_bits.load()),
                                   .end_sample = window.end_sample.load(),
                                   .transport_generation = window.published_generation.load(),
                                   .clipped = window.published_clipped.load()};
    if (before == window.sequence.load())
      return reading;
  }
  return std::nullopt;
}

std::optional<RoutedNodeMeterReading> RoutedNodeMeters::latest(std::size_t node) const noexcept {
  if (node >= size_)
    return std::nullopt;
  const auto& slot = slots_[node];
  const auto published = slot.published.load();
  if (published == 0)
    return std::nullopt;
  return read(slot.windows[(published - 1) % history]);
}

std::optional<RoutedNodeMeterReading>
RoutedNodeMeters::heard(std::size_t node, std::uint64_t heard_sample,
                        std::uint64_t generation) const noexcept {
  if (node >= size_)
    return std::nullopt;
  const auto& slot = slots_[node];
  const auto published = slot.published.load();
  // Newest first; a window the callback overwrote meanwhile fails its read.
  // A graph that switched in ahead of the ear has no heard window yet: its
  // oldest one is the nearest.
  std::optional<RoutedNodeMeterReading> nearest;
  for (std::uint64_t back = 1; back <= std::min<std::uint64_t>(published, history); ++back) {
    const auto reading = read(slot.windows[(published - back) % history]);
    if (!reading || reading->transport_generation != generation)
      continue;
    if (reading->end_sample <= heard_sample)
      return reading;
    nearest = reading;
  }
  return nearest;
}

} // namespace oscillate::engine
