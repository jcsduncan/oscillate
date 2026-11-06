#pragma once

#include "oscillate/media/audio_reader.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>

namespace oscillate::media {

// Nearest project frame, with overflow and rate validation. Available even
// when the optional converter is not built, so import can use one rule.
[[nodiscard]] foundation::Result<std::uint64_t> source_rate_frame_count(std::uint64_t source_frames,
                                                                        std::uint32_t source_rate,
                                                                        std::uint32_t project_rate);
// Metadata-only gate for import, before a staged asset becomes visible.
[[nodiscard]] foundation::Result<void> source_rate_supported(std::uint32_t source_rate,
                                                             std::uint32_t project_rate);

// Bounded, worker-only random-access source conversion. The caller
// must supply an AudioReader opened from a verified source descriptor.
// The caller may copy its project-rate output into a prefetch ring; never call
// this reader from an audio callback.
class SourceRateReader {
public:
  static constexpr std::size_t max_read_frames = 1'024;
  SourceRateReader(const SourceRateReader&) = delete;
  SourceRateReader& operator=(const SourceRateReader&) = delete;
  SourceRateReader(SourceRateReader&&) noexcept;
  SourceRateReader& operator=(SourceRateReader&&) noexcept;
  ~SourceRateReader();

  [[nodiscard]] static foundation::Result<SourceRateReader> open(AudioReader source,
                                                                 std::uint32_t project_rate);
  [[nodiscard]] foundation::Result<std::size_t> read_at(std::uint64_t project_frame,
                                                        std::span<StereoFrame> output,
                                                        const std::stop_token& stop = {});
  [[nodiscard]] std::uint32_t source_rate() const noexcept;
  [[nodiscard]] std::uint32_t project_rate() const noexcept;
  [[nodiscard]] std::uint64_t frame_count() const noexcept;

private:
  struct Backend;
  explicit SourceRateReader(std::unique_ptr<Backend> backend) noexcept;
  std::unique_ptr<Backend> backend_;
};

} // namespace oscillate::media
