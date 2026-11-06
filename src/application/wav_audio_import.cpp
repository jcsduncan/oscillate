#include "oscillate/application/wav_audio_import.hpp"

#include <utility>

namespace oscillate::application {

foundation::Result<PreparedWavAudioImport> WavAudioImportTask::run(const std::stop_token& stop) && {
  auto staged = std::move(stage_).run(stop);
  if (!staged)
    return foundation::Result<PreparedWavAudioImport>::failure(staged.error());
  return foundation::Result<PreparedWavAudioImport>::success(
      PreparedWavAudioImport(std::move(staged).value(), origin_revision_, track_, clip_));
}

} // namespace oscillate::application
