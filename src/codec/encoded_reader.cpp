#include "oscillate/codec/encoded_reader.hpp"

#include <mpg123.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <span>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace oscillate::codec {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
constexpr std::size_t decode_chunk_frames = 2048;
constexpr int max_channels = 2;
constexpr long min_sample_rate = 8'000;
constexpr long max_sample_rate = 384'000;

// mpg123_reader64 (API 48) and mpg123_open_handle64 (API 49, libmpg123 1.33).
static_assert(MPG123_API_VERSION >= 49, "MP3 decoding needs libmpg123 1.33 or later");

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

// libmpg123's reader: the same pinned file and private cursor.
[[nodiscard]] int read_mp3(void* opaque, void* destination, std::size_t count,
                           std::size_t* got) noexcept {
  const auto read = read_file(destination, static_cast<sf_count_t>(count), opaque);
  if (read < 0)
    return -1;
  *got = static_cast<std::size_t>(read);
  return 0;
}

[[nodiscard]] std::int64_t seek_mp3(void* opaque, std::int64_t offset, int whence) noexcept {
  return seek_file(offset, whence, opaque);
}

struct Mp3Delete {
  void operator()(mpg123_handle* handle) const noexcept { mpg123_delete(handle); }
};
using Mp3Handle = std::unique_ptr<mpg123_handle, Mp3Delete>;

// Opens an MP3 for 32-bit float output at its own rate and channel count,
// and scans every frame: without a Xing/Info header the length is otherwise
// an estimate from the file size and the first frame's bitrate, which cuts a
// variable-bitrate stream short.
[[nodiscard]] Result<Mp3Handle> open_mp3(EncodedReader::IoState& io) {
  int error = MPG123_OK;
  Mp3Handle handle(mpg123_new(nullptr, &error));
  const auto refused = [] {
    return Result<Mp3Handle>::failure({ErrorCode::corrupt_data, "Cannot decode MP3 source"});
  };
  if (!handle || error != MPG123_OK)
    return refused();
  const long* rates = nullptr;
  std::size_t rate_count = 0;
  mpg123_rates(&rates, &rate_count);
  if (mpg123_param2(handle.get(), MPG123_ADD_FLAGS, MPG123_QUIET, 0.0) != MPG123_OK ||
      mpg123_format_none(handle.get()) != MPG123_OK)
    return refused();
  for (const auto rate : std::span(rates, rate_count)) {
    if (mpg123_format(handle.get(), rate, MPG123_MONO | MPG123_STEREO, MPG123_ENC_FLOAT_32) !=
        MPG123_OK)
      return refused();
  }
  io.cursor = 0;
  if (mpg123_reader64(handle.get(), read_mp3, seek_mp3, nullptr) != MPG123_OK ||
      mpg123_open_handle64(handle.get(), &io) != MPG123_OK ||
      mpg123_scan(handle.get()) != MPG123_OK)
    return refused();
  return Result<Mp3Handle>::success(std::move(handle));
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
    mp3_ = std::exchange(other.mp3_, nullptr);
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
  if (mp3_ != nullptr) {
    mpg123_delete(mp3_);
    mp3_ = nullptr;
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
  long sample_rate = raw_info.samplerate;
  int channels = raw_info.channels;
  std::uint64_t reported = raw_info.frames > 0 ? static_cast<std::uint64_t>(raw_info.frames) : 0;
  if (format.value() == EncodedFormat::mp3) {
    // libsndfile recognised the stream; libmpg123 decodes it.
    sf_close(reader.handle_);
    reader.handle_ = nullptr;
    auto mp3 = open_mp3(*reader.io_);
    if (!mp3)
      return Result<EncodedReader>::failure(mp3.error());
    reader.mp3_ = std::move(mp3).value().release();
    int encoding = 0;
    const auto length = mpg123_length64(reader.mp3_);
    if (mpg123_getformat(reader.mp3_, &sample_rate, &channels, &encoding) != MPG123_OK ||
        encoding != MPG123_ENC_FLOAT_32)
      return Result<EncodedReader>::failure({ErrorCode::corrupt_data, "Cannot decode MP3 source"});
    reported = length > 0 ? static_cast<std::uint64_t>(length) : 0;
  }
  if (reported == 0 || channels < 1 || channels > max_channels || sample_rate < min_sample_rate ||
      sample_rate > max_sample_rate) {
    return Result<EncodedReader>::failure(
        {ErrorCode::unsupported, "Unsupported encoded audio rate, channel count or length"});
  }
  reader.info_ = {format.value(), static_cast<std::uint64_t>(metadata.st_size), reported,
                  static_cast<std::uint32_t>(sample_rate), static_cast<std::uint32_t>(channels)};
  // An MP3's length may count ID3 tags or a damaged tail as audio.
  if (format.value() == EncodedFormat::mp3)
    reader.info_.frames = reader.decodable_frames(reported);
  if (reader.info_.frames == 0) {
    return Result<EncodedReader>::failure(
        {ErrorCode::corrupt_data, "Encoded audio source has no decodable frames"});
  }
  reader.decoded_cursor_valid_ = false;
  return Result<EncodedReader>::success(std::move(reader));
}

bool EncodedReader::seek(std::uint64_t frame) {
  if (frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return false;
  const auto target = static_cast<std::int64_t>(frame);
  if (mp3_ != nullptr)
    return mpg123_seek64(mp3_, target, SEEK_SET) == target;
  return sf_seek(handle_, target, SEEK_SET) == target;
}

Result<std::size_t> EncodedReader::decode(std::size_t frames) {
  const auto failed = [] {
    return Result<std::size_t>::failure(
        {ErrorCode::io_failure, "Cannot read encoded audio frames"});
  };
  if (mp3_ != nullptr) {
    const auto frame_bytes = sizeof(float) * info_.channels;
    const auto output = std::as_writable_bytes(std::span(scratch_)).first(frames * frame_bytes);
    std::size_t bytes = 0;
    while (bytes < output.size()) {
      std::size_t done = 0;
      const auto status =
          mpg123_read(mp3_, output.subspan(bytes).data(), output.size() - bytes, &done);
      bytes += done;
      if (status == MPG123_DONE || (status == MPG123_OK && done == 0))
        break;
      // The stream may only restate the format it opened with.
      long rate = 0;
      int channels = 0;
      int encoding = 0;
      if (status == MPG123_NEW_FORMAT &&
          mpg123_getformat(mp3_, &rate, &channels, &encoding) == MPG123_OK &&
          std::cmp_equal(rate, info_.sample_rate) && std::cmp_equal(channels, info_.channels) &&
          encoding == MPG123_ENC_FLOAT_32)
        continue;
      if (status != MPG123_OK)
        return failed();
    }
    return Result<std::size_t>::success(bytes / frame_bytes);
  }
  const auto got = sf_readf_float(handle_, scratch_.data(), static_cast<sf_count_t>(frames));
  if (got < 0 || (got == 0 && sf_error(handle_) != SF_ERR_NO_ERROR))
    return failed();
  return Result<std::size_t>::success(static_cast<std::size_t>(got));
}

// Both decoders accept a seek past the real end, and a read there returns
// nothing.
bool EncodedReader::decodes_frame(std::uint64_t frame) {
  if (!seek(frame))
    return false;
  const auto decoded = decode(1);
  return decoded && decoded.value() == 1;
}

// A scanned MP3's length can still count ID3 tags, the encoder's padding or a
// damaged tail as audio. The true length is the first frame that does not
// decode, so the reported length and the decoded stream always agree.
std::uint64_t EncodedReader::decodable_frames(std::uint64_t reported) {
  if (decodes_frame(reported - 1))
    return reported;
  if (!decodes_frame(0))
    return 0;
  // Frame `low` decodes and frame `high` does not.
  std::uint64_t low = 0;
  std::uint64_t high = reported - 1;
  while (high - low > 1) {
    const auto middle = low + (high - low) / 2;
    if (decodes_frame(middle))
      low = middle;
    else
      high = middle;
  }
  return high;
}

Result<std::size_t> EncodedReader::read_at(std::uint64_t source_frame,
                                           std::span<StereoFrame> output) {
  if (source_frame >= info_.frames || output.empty()) {
    return Result<std::size_t>::success(0);
  }
  if (!decoded_cursor_valid_ || source_frame != decoded_cursor_) {
    decoded_cursor_valid_ = false;
    if (!seek(source_frame)) {
      return Result<std::size_t>::failure(
          {ErrorCode::io_failure, "Cannot seek encoded audio source"});
    }
  }
  const auto wanted = std::min<std::uint64_t>(output.size(), info_.frames - source_frame);
  std::size_t decoded = 0;
  while (decoded < wanted) {
    const auto chunk =
        static_cast<std::size_t>(std::min<std::uint64_t>(decode_chunk_frames, wanted - decoded));
    const auto got = decode(chunk);
    if (!got) {
      decoded_cursor_valid_ = false;
      return Result<std::size_t>::failure(got.error());
    }
    for (std::size_t index = 0; index < got.value(); ++index) {
      const auto offset = index * info_.channels;
      output[decoded + index] = {finite_audio(scratch_[offset]),
                                 finite_audio(scratch_[offset + (info_.channels == 2 ? 1 : 0)])};
    }
    decoded += got.value();
    if (got.value() < chunk) {
      break;
    }
  }
  decoded_cursor_ = source_frame + decoded;
  decoded_cursor_valid_ = true;
  return Result<std::size_t>::success(decoded);
}

} // namespace oscillate::codec
