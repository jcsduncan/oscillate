#pragma once

#include "oscillate/application/instrument_playback.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace oscillate::application {

// Why the last start played through the fallback, for the audio settings to
// show. Written by whichever thread starts playback, read by the UI thread.
class OutputFallbackReport {
public:
  void record(std::string reason);
  void clear();
  [[nodiscard]] std::optional<std::string> reason() const;

private:
  mutable std::mutex mutex_;
  std::optional<std::string> reason_;
};

// An output that starts `primary` and, when that cannot start, `fallback`
// instead, so a backend that cannot have its device never leaves playback
// silent (ADR-0130: direct ALSA falling back to PipeWire). Each start tries
// the primary again. Failures to start the primary are recorded in `report`.
[[nodiscard]] PlaybackOutputFactory
make_fallback_output_factory(PlaybackOutputFactory primary, PlaybackOutputFactory fallback,
                             std::shared_ptr<OutputFallbackReport> report);

} // namespace oscillate::application
