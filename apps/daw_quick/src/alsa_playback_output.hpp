#pragma once

#include "oscillate/application/instrument_playback.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace oscillate::quick {

// The ALSA device and period count the audio settings chose, read by the
// output when playback starts, which may be on a worker thread.
class AlsaOutputChoice {
public:
  struct Value {
    std::string device;
    std::uint32_t periods = 0;
  };
  void set(Value value);
  [[nodiscard]] Value get() const;

private:
  mutable std::mutex mutex_;
  Value value_;
};

// Direct ALSA playback (ADR-0130) with the same render source and transport
// boundary as the PipeWire and JACK outputs. The requested buffer is the
// period; the device and period count come from `choice`.
[[nodiscard]] application::PlaybackOutputFactory
make_alsa_playback_output_factory(std::shared_ptr<const AlsaOutputChoice> choice);

} // namespace oscillate::quick
