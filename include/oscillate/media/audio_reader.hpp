#pragma once

#include "oscillate/media/prefetch.hpp"

#include <memory>

namespace oscillate::media {

// Move-only, worker-owned decoder of a verified managed or external source descriptor.
// No method on this type may be called from the audio callback.
class AudioReader {
public:
  AudioReader(const AudioReader&) = delete;
  AudioReader& operator=(const AudioReader&) = delete;
  AudioReader(AudioReader&&) noexcept;
  AudioReader& operator=(AudioReader&&) noexcept;
  ~AudioReader();

  // Duplicates fd. The caller retains and closes its descriptor.
  [[nodiscard]] static foundation::Result<AudioReader> open_fd(int fd,
                                                               const project::SourceRecord& record);
  [[nodiscard]] static AudioReader from_wav(WavReader reader);
  [[nodiscard]] foundation::Result<std::size_t> read_at(std::uint64_t source_frame,
                                                        std::span<StereoFrame> output);
  [[nodiscard]] std::uint32_t sample_rate() const noexcept;
  [[nodiscard]] std::uint32_t channels() const noexcept;
  [[nodiscard]] std::uint64_t frame_count() const noexcept;

private:
  struct Backend;
  explicit AudioReader(std::unique_ptr<Backend> backend) noexcept;
  std::unique_ptr<Backend> backend_;
};

[[nodiscard]] foundation::Result<std::size_t>
fill_from_audio(AudioReader& source, std::uint32_t project_rate, PrefetchRing& ring,
                PrefetchCursor cursor, std::span<StereoFrame> scratch);

} // namespace oscillate::media
