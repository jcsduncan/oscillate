#include "oscillate/codec/encoded_reader.hpp"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <span>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace oscillate::codec {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
constexpr std::size_t decode_chunk_frames = 2048;

[[nodiscard]] sf_count_t file_length(void* opaque) noexcept;
[[nodiscard]] sf_count_t seek_file(sf_count_t offset, int whence, void* opaque) noexcept;
[[nodiscard]] sf_count_t read_file(void* destination, sf_count_t count, void* opaque) noexcept;
[[nodiscard]] sf_count_t write_file(const void*, sf_count_t, void*) noexcept { return 0; }
[[nodiscard]] sf_count_t tell_file(void* opaque) noexcept;

[[nodiscard]] float finite_audio(float value) noexcept {
  return std::isfinite(value) ? value : 0.0F;
}
} // namespace

struct EncodedReader::IoState {
  int fd = -1;
  sf_count_t size = 0;
  sf_count_t cursor = 0;
  ~IoState() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

namespace {
[[nodiscard]] sf_count_t file_length(void* opaque) noexcept {
  return static_cast<EncodedReader::IoState*>(opaque)->size;
}

// Parameter order is fixed by libsndfile's C callback ABI.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] sf_count_t seek_file(sf_count_t offset, int whence, void* opaque) noexcept {
  auto& io = *static_cast<EncodedReader::IoState*>(opaque);
  sf_count_t base = 0;
  if (whence == SEEK_CUR) {
    base = io.cursor;
  } else if (whence == SEEK_END) {
    base = io.size;
  } else if (whence != SEEK_SET) {
    return -1;
  }
  if (offset >= 0) {
    if (offset > io.size - base) {
      return -1;
    }
    io.cursor = base + offset;
  } else {
    // This form handles the most-negative signed offset without negating it.
    const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
    if (magnitude > static_cast<std::uint64_t>(base)) {
      return -1;
    }
    io.cursor = base - static_cast<sf_count_t>(magnitude);
  }
  return io.cursor;
}

[[nodiscard]] sf_count_t read_file(void* destination, sf_count_t count, void* opaque) noexcept {
  auto& io = *static_cast<EncodedReader::IoState*>(opaque);
  if (count <= 0) {
    return 0;
  }
  const auto available = io.size - io.cursor;
  auto remaining = std::min(count, available);
  sf_count_t total = 0;
  auto* bytes = static_cast<unsigned char*>(destination);
  while (remaining > 0) {
    const auto request = static_cast<std::size_t>(std::min<sf_count_t>(remaining, 1 << 20));
    const auto got = ::pread(io.fd, bytes + total, request, static_cast<off_t>(io.cursor));
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      break;
    }
    io.cursor += got;
    total += got;
    remaining -= got;
  }
  return total;
}

[[nodiscard]] sf_count_t tell_file(void* opaque) noexcept {
  return static_cast<EncodedReader::IoState*>(opaque)->cursor;
}

// Whether `frame` exists in the decoded stream. libsndfile accepts a seek
// past the real end, and a read there returns nothing.
[[nodiscard]] bool decodes_frame(SNDFILE* handle, sf_count_t frame, std::span<float> one_frame) {
  return sf_seek(handle, frame, SEEK_SET) == frame &&
         sf_readf_float(handle, one_frame.data(), 1) == 1;
}

// libsndfile takes an MP3's length from its Xing/Info header. Without one it
// estimates the length from the byte count and bitrate, counting ID3 tags and
// the encoder's padding as audio, so the estimate can run past the last
// decodable frame. The true length is the first frame that does not decode.
[[nodiscard]] sf_count_t decodable_frames(SNDFILE* handle, sf_count_t reported,
                                          std::span<float> one_frame) {
  if (decodes_frame(handle, reported - 1, one_frame))
    return reported;
  if (!decodes_frame(handle, 0, one_frame))
    return 0;
  // Frame `low` decodes and frame `high` does not.
  sf_count_t low = 0;
  sf_count_t high = reported - 1;
  while (high - low > 1) {
    const auto middle = low + (high - low) / 2;
    if (decodes_frame(handle, middle, one_frame))
      low = middle;
    else
      high = middle;
  }
  return high;
}

[[nodiscard]] Result<EncodedFormat> classify_format(int format) {
  switch (format & SF_FORMAT_TYPEMASK) {
  case SF_FORMAT_WAV:
    return Result<EncodedFormat>::success(EncodedFormat::wav);
  case SF_FORMAT_AIFF:
    return Result<EncodedFormat>::success(EncodedFormat::aiff);
  case SF_FORMAT_FLAC:
    return Result<EncodedFormat>::success(EncodedFormat::flac);
  case SF_FORMAT_MPEG:
    return Result<EncodedFormat>::success(EncodedFormat::mp3);
  default:
    return Result<EncodedFormat>::failure(
        {ErrorCode::unsupported, "Unsupported encoded audio container"});
  }
}
} // namespace

EncodedReader::EncodedReader(EncodedReader&& other) noexcept { *this = std::move(other); }

EncodedReader& EncodedReader::operator=(EncodedReader&& other) noexcept {
  if (this != &other) {
    close();
    io_ = std::move(other.io_);
    handle_ = std::exchange(other.handle_, nullptr);
    info_ = other.info_;
    decoded_cursor_ = other.decoded_cursor_;
    decoded_cursor_valid_ = std::exchange(other.decoded_cursor_valid_, false);
  }
  return *this;
}

EncodedReader::~EncodedReader() { close(); }

void EncodedReader::close() noexcept {
  if (handle_ != nullptr) {
    sf_close(handle_);
    handle_ = nullptr;
  }
  io_.reset();
  decoded_cursor_valid_ = false;
}

Result<EncodedReader> EncodedReader::open(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) {
    return Result<EncodedReader>::failure(
        {ErrorCode::io_failure, "Cannot open encoded audio source"});
  }
  auto opened = open_fd(fd);
  ::close(fd);
  return opened;
}

Result<EncodedReader> EncodedReader::open_fd(int fd) {
  if (fd < 0) {
    return Result<EncodedReader>::failure(
        {ErrorCode::invalid_argument, "Invalid encoded audio descriptor"});
  }
  EncodedReader reader;
  reader.io_ = std::make_unique<IoState>();
  const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (duplicate < 0) {
    return Result<EncodedReader>::failure(
        {ErrorCode::io_failure, "Cannot duplicate encoded audio descriptor"});
  }
  reader.io_->fd = duplicate;
  struct stat metadata{};
  const auto flags = ::fcntl(duplicate, F_GETFL);
  if (::fstat(duplicate, &metadata) != 0 || !S_ISREG(metadata.st_mode) || flags < 0 ||
      (flags & O_ACCMODE) == O_WRONLY || metadata.st_size <= 0 ||
      static_cast<std::uint64_t>(metadata.st_size) >
          static_cast<std::uint64_t>(std::numeric_limits<sf_count_t>::max())) {
    return Result<EncodedReader>::failure(
        {ErrorCode::unsupported, "Encoded audio must be a readable regular file"});
  }
  reader.io_->size = static_cast<sf_count_t>(metadata.st_size);
  SF_VIRTUAL_IO virtual_io{file_length, seek_file, read_file, write_file, tell_file};
  SF_INFO raw_info{};
  reader.handle_ = sf_open_virtual(&virtual_io, SFM_READ, &raw_info, reader.io_.get());
  if (reader.handle_ == nullptr) {
    return Result<EncodedReader>::failure(
        {ErrorCode::corrupt_data, "Cannot decode encoded audio source"});
  }
  auto format = classify_format(raw_info.format);
  if (!format) {
    return Result<EncodedReader>::failure(format.error());
  }
  if (raw_info.frames <= 0 || raw_info.channels < 1 || raw_info.channels > 2 ||
      raw_info.samplerate < 8'000 || raw_info.samplerate > 384'000) {
    return Result<EncodedReader>::failure(
        {ErrorCode::unsupported, "Unsupported encoded audio rate, channel count or length"});
  }
  const auto frames =
      format.value() == EncodedFormat::mp3
          ? decodable_frames(
                reader.handle_, raw_info.frames,
                std::span(reader.scratch_).first(static_cast<std::size_t>(raw_info.channels)))
          : raw_info.frames;
  if (frames <= 0) {
    return Result<EncodedReader>::failure(
        {ErrorCode::corrupt_data, "Encoded audio source has no decodable frames"});
  }
  reader.info_ = {format.value(), static_cast<std::uint64_t>(metadata.st_size),
                  static_cast<std::uint64_t>(frames),
                  static_cast<std::uint32_t>(raw_info.samplerate),
                  static_cast<std::uint32_t>(raw_info.channels)};
  return Result<EncodedReader>::success(std::move(reader));
}

Result<std::size_t> EncodedReader::read_at(std::uint64_t source_frame,
                                           std::span<StereoFrame> output) {
  if (source_frame >= info_.frames || output.empty()) {
    return Result<std::size_t>::success(0);
  }
  if (!decoded_cursor_valid_ || source_frame != decoded_cursor_) {
    decoded_cursor_valid_ = false;
    if (source_frame > static_cast<std::uint64_t>(std::numeric_limits<sf_count_t>::max()) ||
        sf_seek(handle_, static_cast<sf_count_t>(source_frame), SEEK_SET) < 0) {
      return Result<std::size_t>::failure(
          {ErrorCode::io_failure, "Cannot seek encoded audio source"});
    }
  }
  const auto wanted = std::min<std::uint64_t>(output.size(), info_.frames - source_frame);
  std::size_t decoded = 0;
  while (decoded < wanted) {
    const auto chunk = std::min<std::uint64_t>(decode_chunk_frames, wanted - decoded);
    const auto got = sf_readf_float(handle_, scratch_.data(), static_cast<sf_count_t>(chunk));
    if (got < 0 || (got == 0 && sf_error(handle_) != SF_ERR_NO_ERROR)) {
      decoded_cursor_valid_ = false;
      return Result<std::size_t>::failure(
          {ErrorCode::io_failure, "Cannot read encoded audio frames"});
    }
    for (sf_count_t index = 0; index < got; ++index) {
      const auto offset = static_cast<std::size_t>(index) * info_.channels;
      output[decoded + static_cast<std::size_t>(index)] = {
          finite_audio(scratch_[offset]),
          finite_audio(scratch_[offset + (info_.channels == 2 ? 1 : 0)])};
    }
    decoded += static_cast<std::size_t>(got);
    if (got < static_cast<sf_count_t>(chunk)) {
      break;
    }
  }
  decoded_cursor_ = source_frame + decoded;
  decoded_cursor_valid_ = true;
  return Result<std::size_t>::success(decoded);
}

} // namespace oscillate::codec
