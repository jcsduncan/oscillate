#include "oscillate/export/wav_writer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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

namespace oscillate::exporting {
namespace {

using foundation::Error;
using foundation::ErrorCode;
using foundation::io_error;
using foundation::Result;
using foundation::UniqueFd;

void put16(std::uint8_t* output, std::uint16_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void put32(std::uint8_t* output, std::uint32_t value) noexcept {
  for (unsigned byte = 0; byte < 4; ++byte) {
    output[byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
  }
}

[[nodiscard]] std::uint32_t bytes_per_frame(WavFormat format) noexcept {
  switch (format) {
  case WavFormat::pcm16:
    return 4;
  case WavFormat::pcm24:
    return 6;
  case WavFormat::float32:
    return 8;
  }
  return 0;
}

[[nodiscard]] bool supported_rate(std::uint32_t rate) noexcept {
  return rate == 44'100 || rate == 48'000 || rate == 88'200 || rate == 96'000;
}

constexpr unsigned pcm16_bits = 16;
constexpr unsigned pcm24_bits = 24;

[[nodiscard]] unsigned integer_bits(WavFormat format) noexcept {
  return format == WavFormat::pcm16 ? pcm16_bits : pcm24_bits;
}

[[nodiscard]] std::span<const std::uint8_t>
wav_header(WavSpec spec, std::array<std::uint8_t, 56>& header) noexcept {
  constexpr std::string_view riff = "RIFF";
  constexpr std::string_view wave = "WAVE";
  constexpr std::string_view fmt = "fmt ";
  constexpr std::string_view data = "data";
  constexpr std::string_view fact = "fact";
  const auto floating = spec.format == WavFormat::float32;
  const auto data_offset = floating ? 48U : 36U;
  const auto riff_overhead = floating ? 48U : 36U;
  std::ranges::copy(riff, header.begin());
  std::ranges::copy(wave, header.begin() + 8);
  std::ranges::copy(fmt, header.begin() + 12);
  std::ranges::copy(data, header.begin() + data_offset);
  const auto frame_bytes = bytes_per_frame(spec.format);
  const auto data_bytes = static_cast<std::uint32_t>(spec.expected_frames * frame_bytes);
  put32(header.data() + 4, riff_overhead + data_bytes);
  put32(header.data() + 16, 16);
  put16(header.data() + 20, floating ? 3 : 1);
  put16(header.data() + 22, 2);
  put32(header.data() + 24, spec.sample_rate);
  put32(header.data() + 28, spec.sample_rate * frame_bytes);
  put16(header.data() + 32, static_cast<std::uint16_t>(frame_bytes));
  put16(header.data() + 34, static_cast<std::uint16_t>(frame_bytes * 4));
  if (floating) {
    std::ranges::copy(fact, header.begin() + 36);
    put32(header.data() + 40, 4);
    put32(header.data() + 44, static_cast<std::uint32_t>(spec.expected_frames));
  }
  put32(header.data() + data_offset + 4, data_bytes);
  return std::span(header).first(floating ? 56U : 44U);
}

} // namespace

WavWriter::WavWriter(OpenFiles files, FileNames names, WavSpec spec) noexcept
    : directory_fd_(files.directory), file_fd_(files.file),
      temporary_name_(std::move(names.temporary)), target_name_(std::move(names.target)),
      spec_(spec), header_frames_(spec.expected_frames),
      dither_(integer_bits(spec.format), spec.dither, spec.sample_rate, spec.dither_seed) {}

WavWriter::WavWriter(WavWriter&& other) noexcept
    : directory_fd_(std::move(other.directory_fd_)), file_fd_(std::move(other.file_fd_)),
      temporary_name_(std::move(other.temporary_name_)),
      target_name_(std::move(other.target_name_)), spec_(other.spec_),
      header_frames_(other.header_frames_), frames_written_(other.frames_written_),
      dither_(other.dither_), failed_(other.failed_), published_(other.published_) {
  other.temporary_name_.clear();
}

WavWriter::~WavWriter() { cleanup(); }

void WavWriter::cleanup() noexcept {
  file_fd_.reset();
  if (directory_fd_ && !published_ && !temporary_name_.empty()) {
    (void)::unlinkat(directory_fd_.get(), temporary_name_.c_str(), 0);
  }
  directory_fd_.reset();
}

Result<WavWriter> WavWriter::begin(const std::filesystem::path& target, WavSpec spec) {
  const auto frame_bytes = bytes_per_frame(spec.format);
  if (!supported_rate(spec.sample_rate) || frame_bytes == 0) {
    return Result<WavWriter>::failure(
        {ErrorCode::invalid_argument, "Unsupported WAV format or sample rate"});
  }
  const auto riff_overhead = spec.format == WavFormat::float32 ? 48ULL : 36ULL;
  if (spec.expected_frames >
      (std::numeric_limits<std::uint32_t>::max() - riff_overhead) / frame_bytes) {
    return Result<WavWriter>::failure(
        {ErrorCode::resource_exhausted, "WAV exceeds RIFF size limit"});
  }
  const auto target_name = target.filename().string();
  if (target_name.empty() || target_name == "." || target_name == "..") {
    return Result<WavWriter>::failure({ErrorCode::invalid_argument, "WAV target must name a file"});
  }
  const auto directory =
      target.parent_path().empty() ? std::filesystem::path{"."} : target.parent_path();
  UniqueFd directory_fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!directory_fd) {
    return Result<WavWriter>::failure(io_error("Cannot open WAV output directory"));
  }
  static std::atomic<std::uint64_t> sequence{0};
  int file_fd = -1;
  std::string temp_name;
  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    temp_name = ".oscillate-wav-" + std::to_string(::getpid()) + "-" +
                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
    file_fd = ::openat(directory_fd.get(), temp_name.c_str(),
                       O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (file_fd >= 0 || errno != EEXIST) {
      break;
    }
  }
  if (file_fd < 0) {
    return Result<WavWriter>::failure(io_error("Cannot create WAV temporary file"));
  }
  WavWriter writer({directory_fd.release(), file_fd}, {std::move(temp_name), target_name}, spec);
  std::array<std::uint8_t, 56> header_storage{};
  const auto header = wav_header(spec, header_storage);
  auto written = foundation::write_all(file_fd, std::as_bytes(header), "WAV write failed");
  if (!written) {
    return Result<WavWriter>::failure(written.error());
  }
  return Result<WavWriter>::success(std::move(writer));
}

Result<void> WavWriter::write_interleaved_stereo(std::span<const float> samples) {
  if (failed_ || published_ || !file_fd_) {
    return Result<void>::failure({ErrorCode::conflict, "WAV writer is not writable"});
  }
  if (samples.size() % 2 != 0 || samples.size() / 2 > spec_.expected_frames - frames_written_) {
    failed_ = true;
    return Result<void>::failure({ErrorCode::invalid_argument, "WAV frame count mismatch"});
  }
  if (!std::ranges::all_of(samples, [](float sample) { return std::isfinite(sample); })) {
    failed_ = true;
    return Result<void>::failure({ErrorCode::invalid_argument, "WAV samples must be finite"});
  }
  std::array<std::uint8_t, 8192> bytes{};
  const auto sample_bytes = bytes_per_frame(spec_.format) / 2;
  std::size_t offset = 0;
  while (offset < samples.size()) {
    const auto count = std::min(samples.size() - offset, bytes.size() / sample_bytes);
    for (std::size_t index = 0; index < count; ++index) {
      const auto sample = samples[offset + index];
      auto* output = bytes.data() + index * sample_bytes;
      switch (spec_.format) {
      case WavFormat::pcm16: {
        const auto value = dither_.quantize(sample, (offset + index) % 2);
        put16(output, static_cast<std::uint16_t>(value));
        break;
      }
      case WavFormat::pcm24: {
        const auto value =
            static_cast<std::uint32_t>(dither_.quantize(sample, (offset + index) % 2));
        output[0] = static_cast<std::uint8_t>(value);
        output[1] = static_cast<std::uint8_t>(value >> 8U);
        output[2] = static_cast<std::uint8_t>(value >> 16U);
        break;
      }
      case WavFormat::float32: {
        put32(output, std::bit_cast<std::uint32_t>(sample));
        break;
      }
      }
    }
    auto written = foundation::write_all(
        file_fd_.get(), std::as_bytes(std::span(bytes).first(count * sample_bytes)),
        "WAV write failed");
    if (!written) {
      failed_ = true;
      return written;
    }
    offset += count;
  }
  frames_written_ += samples.size() / 2;
  return Result<void>::success();
}

Result<void> WavWriter::shorten(std::uint64_t frames) {
  if (failed_ || published_ || !file_fd_) {
    return Result<void>::failure({ErrorCode::conflict, "WAV writer is not writable"});
  }
  if (frames < frames_written_ || frames > spec_.expected_frames) {
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "WAV cannot shorten to that length"});
  }
  spec_.expected_frames = frames;
  return Result<void>::success();
}

Result<PublishOutcome> WavWriter::finish() {
  if (failed_ || published_ || !file_fd_) {
    return Result<PublishOutcome>::failure({ErrorCode::conflict, "WAV writer cannot finish"});
  }
  if (frames_written_ != spec_.expected_frames) {
    failed_ = true;
    return Result<PublishOutcome>::failure(
        {ErrorCode::invalid_argument, "WAV render ended before declared frame count"});
  }
  if (header_frames_ != spec_.expected_frames) {
    std::array<std::uint8_t, 56> header_storage{};
    if (::lseek(file_fd_.get(), 0, SEEK_SET) != 0) {
      failed_ = true;
      return Result<PublishOutcome>::failure(io_error("Cannot rewind WAV temporary file"));
    }
    auto written = foundation::write_all(
        file_fd_.get(), std::as_bytes(wav_header(spec_, header_storage)), "WAV write failed");
    if (!written) {
      failed_ = true;
      return Result<PublishOutcome>::failure(written.error());
    }
    header_frames_ = spec_.expected_frames;
  }
  auto published =
      foundation::publish_synced(std::move(file_fd_), directory_fd_.get(), temporary_name_,
                                 target_name_, foundation::PublishMode::replace, "WAV output");
  if (!published) {
    failed_ = true;
    return Result<PublishOutcome>::failure(std::move(published.error()));
  }
  published_ = true;
  return Result<PublishOutcome>::success(published.value() == foundation::Durability::durable
                                             ? PublishOutcome::durable
                                             : PublishOutcome::durability_uncertain);
}

} // namespace oscillate::exporting
