#pragma once

#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/audio_io/pipewire_output.hpp"

namespace oscillate::quick {

// The default device for the app's dry previews (instrument auditions and
// browser samples): a PipeWire stream of its own, beside the project's.
class PreviewPipeWireOutput final : public application::PlaybackOutput {
public:
  PreviewPipeWireOutput(audio_io::StereoRenderSource source, audio_io::TransportCursor& cursor)
      : output_(source, cursor) {}

  [[nodiscard]] foundation::Result<void> start(std::uint32_t rate, std::uint32_t frames,
                                               std::optional<std::uint64_t> serial) override {
    return output_.start(rate, frames, serial);
  }
  void stop() noexcept override { output_.stop(); }
  [[nodiscard]] audio_io::OutputStatus status() const noexcept override { return output_.status(); }

private:
  audio_io::PipeWireOutput output_;
};

} // namespace oscillate::quick
