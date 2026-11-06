#pragma once

#include <oscillate/application/clip_playback.hpp>
#include <oscillate/application/device_control.hpp>

namespace oscillate::application {

// Callback-side permission to advance the transport. Control clears source
// readiness before a quiesced seek and enables it only after the new generation
// is primed. A selected device must also have completed its own ready/primed
// transition. This performs only lock-free atomic reads on the callback.
class PlaybackGate {
public:
  PlaybackGate(const PlaybackReadiness& source,
               const DevicePlaybackControl* device = nullptr) noexcept
      : source_(source), device_(device) {}

  [[nodiscard]] bool enabled() const noexcept {
    return source_.steady() && (device_ == nullptr || device_->transport_enabled());
  }

private:
  const PlaybackReadiness& source_;
  const DevicePlaybackControl* device_;
};

} // namespace oscillate::application
