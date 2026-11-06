#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "oscillate/foundation/split_mix.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace oscillate::exporting {

enum class WavFormat : std::uint8_t { pcm16, pcm24, float32 };

struct WavSpec {
  std::uint32_t sample_rate = 48'000;
  WavFormat format = WavFormat::float32;
  std::uint64_t expected_frames = 0;
  std::uint64_t dither_seed = 0;
};

// A renamed output exists, but its containing directory could not be synced.
// The caller must report this separately from a failure before publication.
enum class PublishOutcome : std::uint8_t { durable, durability_uncertain };

// Control/export-worker thread only. Samples are interleaved left, right.
// An unfinished writer removes its temporary file and never changes target.
class WavWriter {
public:
  [[nodiscard]] static foundation::Result<WavWriter> begin(const std::filesystem::path& target,
                                                           WavSpec spec);

  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;
  WavWriter(WavWriter&& other) noexcept;
  WavWriter& operator=(WavWriter&& other) = delete;
  ~WavWriter();

  [[nodiscard]] foundation::Result<void> write_interleaved_stereo(std::span<const float> samples);
  // Ends the file at `frames` (written so far or more, no more than
  // declared): finish() then rewrites the header for the shorter length.
  [[nodiscard]] foundation::Result<void> shorten(std::uint64_t frames);
  [[nodiscard]] foundation::Result<PublishOutcome> finish();
  [[nodiscard]] std::uint64_t frames_written() const noexcept { return frames_written_; }

private:
  struct OpenFiles {
    int directory;
    int file;
  };
  struct FileNames {
    std::string temporary;
    std::string target;
  };
  WavWriter(OpenFiles files, FileNames names, WavSpec spec) noexcept;
  void cleanup() noexcept;

  foundation::UniqueFd directory_fd_;
  foundation::UniqueFd file_fd_;
  std::string temporary_name_;
  std::string target_name_;
  WavSpec spec_;
  // The length the header on disk declares.
  std::uint64_t header_frames_ = 0;
  std::uint64_t frames_written_ = 0;
  foundation::SplitMix64 dither_;
  bool failed_ = false;
  bool published_ = false;
};

} // namespace oscillate::exporting
