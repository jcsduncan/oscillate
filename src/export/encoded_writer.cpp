#include "encoded_writer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <ranges>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace oscillate::exporting::detail {
namespace {
using foundation::Error;
using foundation::ErrorCode;
using foundation::io_error;
using foundation::Result;
using foundation::UniqueFd;

[[nodiscard]] Error codec_error(std::string_view action, SNDFILE* handle) {
  return {ErrorCode::io_failure, std::string(action) + ": " + sf_strerror(handle)};
}

[[nodiscard]] bool supported_rate(std::uint32_t rate) noexcept {
  return rate == 44'100 || rate == 48'000 || rate == 88'200 || rate == 96'000;
}

template <unsigned Bits>
[[nodiscard]] std::int32_t quantize(float sample, foundation::SplitMix64& dither) noexcept {
  const auto scale = static_cast<double>(std::uint32_t{1} << (Bits - 1U));
  const auto noise = dither.unit() - dither.unit();
  const auto quantized = std::round(static_cast<double>(sample) * scale + noise);
  return static_cast<std::int32_t>(std::clamp(quantized, -scale, scale - 1.0));
}

[[nodiscard]] int sndfile_format(EncodedMasterFormat format) noexcept {
  switch (format) {
  case EncodedMasterFormat::flac16:
    return SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
  case EncodedMasterFormat::flac24:
    return SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
  case EncodedMasterFormat::mp3:
    return SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
  }
  return 0;
}
} // namespace

EncodedWriter::EncodedWriter(OpenFiles files, FileNames names, SNDFILE* handle, Spec spec) noexcept
    : directory_fd_(files.directory), file_fd_(files.file),
      temporary_name_(std::move(names.temporary)), target_name_(std::move(names.target)),
      handle_(handle), format_(spec.format), sample_rate_(spec.sample_rate),
      expected_frames_(spec.expected_frames), dither_(spec.dither_seed) {}

EncodedWriter::EncodedWriter(EncodedWriter&& other) noexcept
    : directory_fd_(std::move(other.directory_fd_)), file_fd_(std::move(other.file_fd_)),
      temporary_name_(std::move(other.temporary_name_)),
      target_name_(std::move(other.target_name_)), handle_(std::exchange(other.handle_, nullptr)),
      format_(other.format_), sample_rate_(other.sample_rate_),
      expected_frames_(other.expected_frames_), frames_written_(other.frames_written_),
      dither_(other.dither_), failed_(other.failed_), published_(other.published_) {
  other.temporary_name_.clear();
}

EncodedWriter::~EncodedWriter() { cleanup(); }

void EncodedWriter::cleanup() noexcept {
  if (handle_ != nullptr) {
    (void)::sf_close(handle_);
    handle_ = nullptr;
  }
  file_fd_.reset();
  if (directory_fd_ && !published_ && !temporary_name_.empty()) {
    (void)::unlinkat(directory_fd_.get(), temporary_name_.c_str(), 0);
  }
  directory_fd_.reset();
}

Result<EncodedWriter> EncodedWriter::begin(const EncodedMasterOptions& options,
                                           std::uint32_t sample_rate,
                                           std::uint64_t expected_frames) {
  const auto format = sndfile_format(options.format);
  if (format == 0 || !supported_rate(sample_rate) || expected_frames == 0 ||
      expected_frames > std::numeric_limits<std::int64_t>::max()) {
    return Result<EncodedWriter>::failure(
        {ErrorCode::invalid_argument, "Unsupported encoded master format, rate or frame count"});
  }
  if (options.format == EncodedMasterFormat::mp3 && sample_rate > 48'000) {
    return Result<EncodedWriter>::failure(
        {ErrorCode::unsupported, "MP3 master supports 44.1 or 48 kHz only"});
  }
  if (!std::isfinite(options.compression_level) || options.compression_level < 0.0 ||
      options.compression_level > 1.0) {
    return Result<EncodedWriter>::failure(
        {ErrorCode::invalid_argument, "Encoded compression level must be within [0, 1]"});
  }
  SF_INFO info{};
  info.samplerate = static_cast<int>(sample_rate);
  info.channels = 2;
  info.format = format;
  if (::sf_format_check(&info) != SF_TRUE) {
    return Result<EncodedWriter>::failure(
        {ErrorCode::unsupported, "Installed libsndfile cannot encode selected master format"});
  }
  const auto target_name = options.target.filename().string();
  if (target_name.empty() || target_name == "." || target_name == "..") {
    return Result<EncodedWriter>::failure(
        {ErrorCode::invalid_argument, "Encoded master target must name a file"});
  }
  const auto extension = options.format == EncodedMasterFormat::mp3 ? ".mp3" : ".flac";
  if (options.target.extension() != extension) {
    return Result<EncodedWriter>::failure(
        {ErrorCode::invalid_argument, "Encoded master target extension does not match format"});
  }
  const auto directory = options.target.parent_path().empty() ? std::filesystem::path{"."}
                                                              : options.target.parent_path();
  UniqueFd directory_fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory_fd) {
    return Result<EncodedWriter>::failure(io_error("Cannot open encoded output directory"));
  }
  static std::atomic<std::uint64_t> sequence{0};
  int file_fd = -1;
  std::string temporary_name;
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    temporary_name = ".oscillate-encoded-" + std::to_string(::getpid()) + "-" +
                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    file_fd = ::openat(directory_fd.get(), temporary_name.c_str(),
                       O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (file_fd >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (file_fd < 0) {
    return Result<EncodedWriter>::failure(io_error("Cannot create encoded temporary file"));
  }
  auto* handle = ::sf_open_fd(file_fd, SFM_WRITE, &info, SF_FALSE);
  if (handle == nullptr) {
    const auto error = codec_error("Cannot open encoded master writer", nullptr);
    (void)::close(file_fd);
    (void)::unlinkat(directory_fd.get(), temporary_name.c_str(), 0);
    return Result<EncodedWriter>::failure(error);
  }
  EncodedWriter writer({directory_fd.release(), file_fd}, {std::move(temporary_name), target_name},
                       handle, {options.format, sample_rate, expected_frames, options.dither_seed});
  if (options.format == EncodedMasterFormat::mp3) {
    double level = options.compression_level;
    if (::sf_command(handle, SFC_SET_COMPRESSION_LEVEL, &level, sizeof(level)) != SF_TRUE) {
      return Result<EncodedWriter>::failure(
          {ErrorCode::unsupported, "Installed MP3 encoder cannot set requested quality"});
    }
  }
  return Result<EncodedWriter>::success(std::move(writer));
}

Result<void> EncodedWriter::write_interleaved_stereo(std::span<const float> samples) {
  if (failed_ || published_ || handle_ == nullptr) {
    return Result<void>::failure({ErrorCode::conflict, "Encoded master writer is not writable"});
  }
  if (samples.size() % 2 != 0 || samples.size() / 2 > expected_frames_ - frames_written_) {
    failed_ = true;
    return Result<void>::failure({ErrorCode::invalid_argument, "Encoded frame count mismatch"});
  }
  if (!std::ranges::all_of(samples, [](float sample) { return std::isfinite(sample); })) {
    failed_ = true;
    return Result<void>::failure({ErrorCode::invalid_argument, "Encoded samples must be finite"});
  }
  constexpr std::size_t chunk_frames = 1'024;
  std::array<short, chunk_frames * 2> pcm16{};
  std::array<int, chunk_frames * 2> pcm24{};
  for (std::size_t first = 0; first < samples.size() / 2;) {
    const auto count = std::min(chunk_frames, samples.size() / 2 - first);
    sf_count_t written = 0;
    switch (format_) {
    case EncodedMasterFormat::flac16:
      for (std::size_t index = 0; index < count * 2; ++index) {
        pcm16[index] = static_cast<short>(quantize<16>(samples[first * 2 + index], dither_));
      }
      written = ::sf_writef_short(handle_, pcm16.data(), static_cast<sf_count_t>(count));
      break;
    case EncodedMasterFormat::flac24:
      for (std::size_t index = 0; index < count * 2; ++index) {
        pcm24[index] = quantize<24>(samples[first * 2 + index], dither_) * 256;
      }
      written = ::sf_writef_int(handle_, pcm24.data(), static_cast<sf_count_t>(count));
      break;
    case EncodedMasterFormat::mp3:
      written =
          ::sf_writef_float(handle_, samples.data() + first * 2, static_cast<sf_count_t>(count));
      break;
    }
    if (written != static_cast<sf_count_t>(count)) {
      failed_ = true;
      return Result<void>::failure(codec_error("Encoded master write failed", handle_));
    }
    first += count;
  }
  frames_written_ += samples.size() / 2;
  return Result<void>::success();
}

Result<void> EncodedWriter::shorten(std::uint64_t frames) {
  if (failed_ || published_ || handle_ == nullptr) {
    return Result<void>::failure({ErrorCode::conflict, "Encoded master writer is not writable"});
  }
  if (frames < frames_written_ || frames > expected_frames_ || frames == 0) {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Encoded master cannot shorten to that length"});
  }
  expected_frames_ = frames;
  return Result<void>::success();
}

Result<PublishOutcome> EncodedWriter::finish(const std::function<void(int)>& after_encoder_close) {
  if (failed_ || published_ || handle_ == nullptr) {
    return Result<PublishOutcome>::failure(
        {ErrorCode::conflict, "Encoded master writer cannot finish"});
  }
  if (frames_written_ != expected_frames_) {
    failed_ = true;
    return Result<PublishOutcome>::failure(
        {ErrorCode::invalid_argument, "Encoded render ended before declared frame count"});
  }
  if (::sf_close(handle_) != 0) {
    handle_ = nullptr;
    failed_ = true;
    return Result<PublishOutcome>::failure(
        {ErrorCode::io_failure, "Cannot finalize encoded master"});
  }
  handle_ = nullptr;
  if (after_encoder_close) {
    after_encoder_close(file_fd_.get());
  }
  if (::lseek(file_fd_.get(), 0, SEEK_SET) < 0) {
    failed_ = true;
    return Result<PublishOutcome>::failure(io_error("Cannot rewind encoded temporary file"));
  }
  SF_INFO inspected{};
  auto* inspection = ::sf_open_fd(file_fd_.get(), SFM_READ, &inspected, SF_FALSE);
  if (inspection == nullptr) {
    failed_ = true;
    return Result<PublishOutcome>::failure(codec_error("Cannot verify encoded output", nullptr));
  }
  std::array<float, 2> probe{};
  const bool valid = inspected.format == sndfile_format(format_) && inspected.channels == 2 &&
                     inspected.samplerate == static_cast<int>(sample_rate_) &&
                     inspected.frames == static_cast<sf_count_t>(expected_frames_) &&
                     ::sf_readf_float(inspection, probe.data(), 1) == 1 &&
                     ::sf_seek(inspection, static_cast<sf_count_t>(expected_frames_ - 1),
                               SEEK_SET) == static_cast<sf_count_t>(expected_frames_ - 1) &&
                     ::sf_readf_float(inspection, probe.data(), 1) == 1;
  const auto inspect_close = ::sf_close(inspection);
  if (!valid || inspect_close != 0) {
    failed_ = true;
    return Result<PublishOutcome>::failure(
        {ErrorCode::corrupt_data, "Encoded output failed metadata or endpoint verification"});
  }
  auto published =
      foundation::publish_synced(std::move(file_fd_), directory_fd_.get(), temporary_name_,
                                 target_name_, foundation::PublishMode::replace, "encoded master");
  if (!published) {
    failed_ = true;
    return Result<PublishOutcome>::failure(std::move(published.error()));
  }
  published_ = true;
  return Result<PublishOutcome>::success(published.value() == foundation::Durability::durable
                                             ? PublishOutcome::durable
                                             : PublishOutcome::durability_uncertain);
}

} // namespace oscillate::exporting::detail
