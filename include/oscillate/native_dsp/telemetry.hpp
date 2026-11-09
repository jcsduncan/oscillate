#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::native_dsp {

// Live state for panels (ADR-0037): a short list of floats whose layout each
// engine documents, written by the callback after every block.
inline constexpr std::size_t max_telemetry_values = 2'048;

// A lock-free single-producer (callback), single-consumer (control) triple
// buffer of telemetry frames. The producer fills its back slot and swaps it
// with the middle; the consumer swaps the middle with its front slot when a
// new frame is waiting, so neither side blocks or sees a torn frame.
class TelemetryBuffer {
public:
  // Callback only: `fill` writes up to max_telemetry_values and returns how
  // many; zero publishes nothing.
  template <typename Fill> void publish(Fill&& fill) noexcept {
    auto& slot = frames_[back_];
    slot.size = fill(std::span(slot.values));
    if (slot.size == 0) {
      return;
    }
    const auto previous =
        middle_.exchange(static_cast<std::uint8_t>(back_ | fresh), std::memory_order_acq_rel);
    back_ = static_cast<std::uint8_t>(previous & 3U);
  }
  // Control only: the newest complete frame, false until the first.
  [[nodiscard]] bool read(std::vector<float>& out);

private:
  struct Slot {
    std::array<float, max_telemetry_values> values{};
    std::size_t size = 0;
  };
  static constexpr std::uint8_t fresh = 4;
  std::array<Slot, 3> frames_{}; // Not "slots": Qt defines that as a macro.
  std::atomic<std::uint8_t> middle_{1};
  std::uint8_t back_ = 0;  // Callback only.
  std::uint8_t front_ = 2; // Control only.
  bool any_ = false;       // Control only: a frame has been read.
};

} // namespace oscillate::native_dsp
