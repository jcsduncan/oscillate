#pragma once

#include "oscillate/foundation/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

struct sf_private_tag;
struct mpg123_handle_struct;

namespace oscillate::codec {

enum class EncodedFormat : std::uint8_t { wav, aiff, flac, mp3 };

struct StereoFrame {
  float left = 0.0F;
  float right = 0.0F;
};

struct SourceInfo {
  EncodedFormat format;
  std::uint64_t encoded_bytes;
  std::uint64_t frames;
  std::uint32_t sample_rate;
  std::uint32_t channels;
};

// Control/worker-only decoder. Virtual I/O keeps a private cursor over a
// pinned regular file, so a caller's descriptor offset and path are untouched.
// libsndfile decodes WAV, AIFF and FLAC; libmpg123 decodes MP3, scanning the
// whole stream for its length.
class EncodedReader {
public:
  // Opaque virtual-I/O state. Exposed as a type only for the C callbacks.
  struct IoState;
  EncodedReader(const EncodedReader&) = delete;
  EncodedReader& operator=(const EncodedReader&) = delete;
  EncodedReader(EncodedReader&& other) noexcept;
  EncodedReader& operator=(EncodedReader&& other) noexcept;
  ~EncodedReader();

  [[nodiscard]] static foundation::Result<EncodedReader> open(const std::filesystem::path& path);
  [[nodiscard]] static foundation::Result<EncodedReader> open_fd(int fd);
  [[nodiscard]] const SourceInfo& info() const noexcept { return info_; }
  [[nodiscard]] foundation::Result<std::size_t> read_at(std::uint64_t source_frame,
                                                        std::span<StereoFrame> output);

private:
  EncodedReader() = default;
  void close() noexcept;
  // Decodes up to `frames` frames at the decoder's position into scratch_;
  // fewer only at the end of the stream.
  [[nodiscard]] foundation::Result<std::size_t> decode(std::size_t frames);
  [[nodiscard]] bool seek(std::uint64_t frame);
  // Whether `frame` exists in the decoded stream.
  [[nodiscard]] bool decodes_frame(std::uint64_t frame);
  // The first frame that does not decode, from a length that may overstate it.
  [[nodiscard]] std::uint64_t decodable_frames(std::uint64_t reported);

  std::unique_ptr<IoState> io_;
  // One of the two is open.
  sf_private_tag* handle_ = nullptr;
  mpg123_handle_struct* mp3_ = nullptr;
  SourceInfo info_{};
  // Worker-owned bounded decode scratch; reused across prefetch fills.
  std::array<float, 4096> scratch_{};
  std::uint64_t decoded_cursor_ = 0;
  bool decoded_cursor_valid_ = false;
};

} // namespace oscillate::codec
