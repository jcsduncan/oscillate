#pragma once

#include "oscillate/export/offline_master.hpp"
#include "oscillate/foundation/file_io.hpp"
#include "oscillate/foundation/split_mix.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

#include <sndfile.h>

namespace oscillate::exporting::detail {

// Worker-only, codec-build output. Like WavWriter, an unfinished instance
// removes its private temporary file without changing the requested target.
class EncodedWriter {
public:
  [[nodiscard]] static foundation::Result<EncodedWriter> begin(const EncodedMasterOptions& options,
                                                               std::uint32_t sample_rate,
                                                               std::uint64_t expected_frames);

  EncodedWriter(const EncodedWriter&) = delete;
  EncodedWriter& operator=(const EncodedWriter&) = delete;
  EncodedWriter(EncodedWriter&& other) noexcept;
  EncodedWriter& operator=(EncodedWriter&& other) = delete;
  ~EncodedWriter();

  [[nodiscard]] foundation::Result<void> write_interleaved_stereo(std::span<const float> samples);
  // Ends the file at `frames` (written so far or more, no more than declared).
  [[nodiscard]] foundation::Result<void> shorten(std::uint64_t frames);
  // Fault seam for an interrupted/corrupt finalized encoder file. Production
  // passes no hook; the callback runs only on the export worker.
  [[nodiscard]] foundation::Result<PublishOutcome>
  finish(const std::function<void(int)>& after_encoder_close = {});

private:
  struct OpenFiles {
    int directory;
    int file;
  };
  struct FileNames {
    std::string temporary;
    std::string target;
  };
  struct Spec {
    EncodedMasterFormat format;
    std::uint32_t sample_rate;
    std::uint64_t expected_frames;
    std::uint64_t dither_seed;
  };
  EncodedWriter(OpenFiles files, FileNames names, SNDFILE* handle, Spec spec) noexcept;
  void cleanup() noexcept;

  foundation::UniqueFd directory_fd_;
  foundation::UniqueFd file_fd_;
  std::string temporary_name_;
  std::string target_name_;
  SNDFILE* handle_ = nullptr;
  EncodedMasterFormat format_ = EncodedMasterFormat::flac24;
  std::uint32_t sample_rate_ = 0;
  std::uint64_t expected_frames_ = 0;
  std::uint64_t frames_written_ = 0;
  foundation::SplitMix64 dither_;
  bool failed_ = false;
  bool published_ = false;
};

} // namespace oscillate::exporting::detail
