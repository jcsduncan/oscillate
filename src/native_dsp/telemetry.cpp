#include "oscillate/native_dsp/telemetry.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace oscillate::native_dsp {

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

} // namespace oscillate::native_dsp
