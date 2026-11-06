#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace oscillate::application {

enum class DevicePlaybackState : std::uint8_t {
  stopped,
  awaiting_device,
  priming,
  playing,
  needs_reselection
};

enum class DeviceLossReason : std::uint8_t { none, target_lost, invalid_format };

enum class DeviceEventKind : std::uint8_t { ready, target_lost, format_invalid };

// The device adapter supplies an event only after identifying the selected
// target and, for ready, validating its negotiated audio format.
struct DeviceEvent {
  DeviceEventKind kind;
  std::uint64_t target_serial;
  std::uint64_t selection_epoch;
};

struct DeviceControlStatus {
  DevicePlaybackState state = DevicePlaybackState::stopped;
  DeviceLossReason loss_reason = DeviceLossReason::none;
  std::optional<std::uint64_t> target_serial;
  std::uint64_t selection_epoch = 0;
};

// Control-thread state machine. Only transport_enabled() may be read by the
// audio callback. The caller must check that gate before advancing its
// transport cursor; all events and other methods run off the callback.
class DevicePlaybackControl {
public:
  // A new selection, including retrying the same target, needs fresh device
  // readiness and source priming. Zero and exhausted epochs are rejected.
  [[nodiscard]] std::optional<std::uint64_t> select_target(std::uint64_t target_serial) noexcept;
  [[nodiscard]] bool observe(DeviceEvent event) noexcept;
  [[nodiscard]] bool primed(std::uint64_t selection_epoch) noexcept;
  void stop() noexcept;

  [[nodiscard]] DeviceControlStatus status() const noexcept { return status_; }
  [[nodiscard]] bool transport_enabled() const noexcept {
    return transport_enabled_.load(std::memory_order_acquire);
  }

private:
  static_assert(std::atomic<bool>::is_always_lock_free);
  std::atomic<bool> transport_enabled_{false};
  DeviceControlStatus status_;
};

} // namespace oscillate::application
