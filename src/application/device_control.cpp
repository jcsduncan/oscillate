#include <oscillate/application/device_control.hpp>

#include <limits>

namespace oscillate::application {

std::optional<std::uint64_t>
DevicePlaybackControl::select_target(std::uint64_t target_serial) noexcept {
  if (target_serial == 0 || status_.selection_epoch == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  transport_enabled_.store(false, std::memory_order_release);
  ++status_.selection_epoch;
  status_.target_serial = target_serial;
  status_.loss_reason = DeviceLossReason::none;
  status_.state = DevicePlaybackState::awaiting_device;
  return status_.selection_epoch;
}

bool DevicePlaybackControl::observe(DeviceEvent event) noexcept {
  if (!status_.target_serial || event.target_serial != *status_.target_serial ||
      event.selection_epoch != status_.selection_epoch ||
      status_.state == DevicePlaybackState::stopped ||
      status_.state == DevicePlaybackState::needs_reselection) {
    return false;
  }
  switch (event.kind) {
  case DeviceEventKind::ready:
    if (status_.state != DevicePlaybackState::awaiting_device) {
      return false;
    }
    status_.state = DevicePlaybackState::priming;
    return true;
  case DeviceEventKind::target_lost:
    transport_enabled_.store(false, std::memory_order_release);
    status_.loss_reason = DeviceLossReason::target_lost;
    status_.state = DevicePlaybackState::needs_reselection;
    return true;
  case DeviceEventKind::format_invalid:
    transport_enabled_.store(false, std::memory_order_release);
    status_.loss_reason = DeviceLossReason::invalid_format;
    status_.state = DevicePlaybackState::needs_reselection;
    return true;
  }
  return false;
}

bool DevicePlaybackControl::primed(std::uint64_t selection_epoch) noexcept {
  if (selection_epoch != status_.selection_epoch || status_.state != DevicePlaybackState::priming) {
    return false;
  }
  status_.state = DevicePlaybackState::playing;
  transport_enabled_.store(true, std::memory_order_release);
  return true;
}

void DevicePlaybackControl::stop() noexcept {
  transport_enabled_.store(false, std::memory_order_release);
  status_.target_serial.reset();
  status_.loss_reason = DeviceLossReason::none;
  status_.state = DevicePlaybackState::stopped;
}

} // namespace oscillate::application
