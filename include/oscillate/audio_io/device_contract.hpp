#pragma once

#include <atomic>
#include <cstdint>

namespace oscillate::audio_io {

enum class OutputState : std::uint8_t {
  stopped,
  connecting,
  paused,
  streaming,
  disconnected,
  error
};

// Optional callback-safe playback gate supplied by the application. Returning
// false emits silence without advancing transport or counting a render failure.
// The callback function and its context must remain valid until stop returns.
struct TransportPermission {
  const void* context = nullptr;
  bool (*enabled)(const void*) noexcept = nullptr;
};

// A stop request can suppress callback work before the control thread enters
// PipeWire's blocking teardown. It cannot preempt a render already in flight
// or establish a deadline for joining the device thread.
class CallbackStopGate {
public:
  void reset_after_quiesce() noexcept { requested_.store(false, std::memory_order_release); }
  void request() noexcept { requested_.store(true, std::memory_order_release); }
  [[nodiscard]] bool requested() const noexcept {
    return requested_.load(std::memory_order_acquire);
  }

private:
  std::atomic<bool> requested_{false};
  static_assert(std::atomic<bool>::is_always_lock_free);
};

} // namespace oscillate::audio_io
