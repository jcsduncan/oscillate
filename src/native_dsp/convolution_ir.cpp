#include "oscillate/native_dsp/convolution_ir.hpp"

#include "oscillate/media/prefetch.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace oscillate::native_dsp {
namespace {

using foundation::ErrorCode;
using foundation::Result;

constexpr std::uint64_t max_encoded_bytes = 16ULL * 1024 * 1024;
// Measured two-second 48 kHz processor state is about 3.4 MiB. Scale the
// 6 MiB admission reserve with supported sample rate because the maximum IR
// doubles in frames by 96 kHz. This is an estimate, not an allocator quota.
[[nodiscard]] constexpr std::uint64_t preparation_reserve_bytes(std::uint32_t rate) noexcept {
  constexpr std::uint64_t reserve_at_48k = 6ULL * 1024 * 1024;
  return reserve_at_48k * std::max(rate, 48'000U) / 48'000U;
}

[[nodiscard]] bool same_version(const struct stat& lhs, const struct stat& rhs) noexcept {
  return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino && lhs.st_size == rhs.st_size &&
         lhs.st_mtim.tv_sec == rhs.st_mtim.tv_sec && lhs.st_mtim.tv_nsec == rhs.st_mtim.tv_nsec &&
         lhs.st_ctim.tv_sec == rhs.st_ctim.tv_sec && lhs.st_ctim.tv_nsec == rhs.st_ctim.tv_nsec;
}

[[nodiscard]] bool valid_sha256(const std::string& value) noexcept {
  return value.size() == 64 && std::ranges::all_of(value, [](char digit) {
           return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
         });
}

[[nodiscard]] Result<std::string> hash_fd(int fd, const ConvolutionIrIdentity& identity) {
  using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  Context context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    return Result<std::string>::failure({ErrorCode::internal, "IR digest initialization failed"});
  }
  std::array<unsigned char, 8192> scratch{};
  for (std::uint64_t offset = 0; offset < identity.encoded_bytes;) {
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(scratch.size(), identity.encoded_bytes - offset));
    const auto read = ::pread(fd, scratch.data(), count, static_cast<off_t>(offset));
    if (read < 0 && errno == EINTR) {
      continue;
    }
    if (read <= 0) {
      return Result<std::string>::failure(
          {ErrorCode::io_failure, "IR source changed during digest"});
    }
    if (EVP_DigestUpdate(context.get(), scratch.data(), static_cast<std::size_t>(read)) != 1) {
      return Result<std::string>::failure({ErrorCode::internal, "IR digest update failed"});
    }
    offset += static_cast<std::uint64_t>(read);
  }
  std::array<unsigned char, 32> raw{};
  unsigned length = 0;
  if (EVP_DigestFinal_ex(context.get(), raw.data(), &length) != 1 || length != raw.size()) {
    return Result<std::string>::failure({ErrorCode::internal, "IR digest finalization failed"});
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (const auto byte : raw) {
    hex.push_back(digits[byte >> 4U]);
    hex.push_back(digits[byte & 0x0fU]);
  }
  return Result<std::string>::success(std::move(hex));
}

[[nodiscard]] Result<PreparedConvolutionIr>
prepare_ir(int fd, const ConvolutionIrIdentity& expected, std::uint64_t preparation_bytes,
           const ConvolutionIrBudget* live_budget, std::uint32_t project_rate, float wet) {
  if (fd < 0 || !valid_sha256(expected.sha256) || expected.encoded_bytes < 44 ||
      expected.encoded_bytes > max_encoded_bytes ||
      (expected.channels != 1 && expected.channels != 2) || expected.sample_rate != project_rate ||
      (project_rate != 44'100 && project_rate != 48'000 && project_rate != 88'200 &&
       project_rate != 96'000) ||
      expected.frames == 0 || expected.frames > 2ULL * project_rate || !std::isfinite(wet) ||
      wet < 0.0F || wet > 1.0F) {
    return Result<PreparedConvolutionIr>::failure(
        {ErrorCode::invalid_argument, "IR identity, rate, duration or wet level is invalid"});
  }
  if (preparation_bytes < preparation_reserve_bytes(project_rate)) {
    return Result<PreparedConvolutionIr>::failure(
        {ErrorCode::resource_exhausted, "convolution IR exceeds preparation memory admission"});
  }
  if (live_budget != nullptr &&
      (live_budget->active_instances != 0 || live_budget->measured_worst_callback_us == 0 ||
       live_budget->available_callback_us == 0 ||
       live_budget->measured_ir_sha256 != expected.sha256 ||
       live_budget->measured_ir_frames != expected.frames ||
       live_budget->measured_sample_rate != project_rate ||
       (live_budget->measured_block_frames != 64 && live_budget->measured_block_frames != 128) ||
       live_budget->measured_worst_callback_us > live_budget->available_callback_us / 2)) {
    return Result<PreparedConvolutionIr>::failure(
        {ErrorCode::resource_exhausted,
         "convolution IR exceeds measured CPU or memory admission budget"});
  }
  struct stat before{};
  if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
      static_cast<std::uint64_t>(before.st_size) != expected.encoded_bytes) {
    return Result<PreparedConvolutionIr>::failure(
        {ErrorCode::corrupt_data, "IR source is not the recorded regular file"});
  }
  auto hash = hash_fd(fd, expected);
  if (!hash) {
    return Result<PreparedConvolutionIr>::failure(std::move(hash.error()));
  }
  if (hash.value() != expected.sha256) {
    return Result<PreparedConvolutionIr>::failure(
        {ErrorCode::corrupt_data, "IR source checksum differs from recorded identity"});
  }
  auto reader = media::WavReader::open_fd(fd);
  if (!reader) {
    return Result<PreparedConvolutionIr>::failure(std::move(reader.error()));
  }
  if (reader.value().sample_rate() != expected.sample_rate ||
      reader.value().channels() != expected.channels ||
      reader.value().frame_count() != expected.frames) {
    return Result<PreparedConvolutionIr>::failure(
        {ErrorCode::corrupt_data, "IR WAV metadata differs from recorded identity"});
  }
  std::vector<engine::StereoFrame> frames;
  try {
    frames.resize(static_cast<std::size_t>(expected.frames));
  } catch (const std::bad_alloc&) {
    return Result<PreparedConvolutionIr>::failure(
        {ErrorCode::resource_exhausted, "IR preparation memory allocation failed"});
  }
  std::array<media::StereoFrame, 4096> scratch{};
  for (std::uint64_t offset = 0; offset < expected.frames;) {
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(scratch.size(), expected.frames - offset));
    auto count = reader.value().read_at(offset, std::span(scratch).first(wanted));
    if (!count) {
      return Result<PreparedConvolutionIr>::failure(std::move(count.error()));
    }
    if (count.value() != wanted) {
      return Result<PreparedConvolutionIr>::failure(
          {ErrorCode::io_failure, "IR data is incomplete"});
    }
    for (std::size_t index = 0; index < wanted; ++index) {
      frames[static_cast<std::size_t>(offset) + index] = {scratch[index].left,
                                                          scratch[index].right};
    }
    offset += wanted;
  }
  struct stat after{};
  auto final_hash = hash_fd(fd, expected);
  if (!final_hash) {
    return Result<PreparedConvolutionIr>::failure(std::move(final_hash.error()));
  }
  if (::fstat(fd, &after) != 0 || !same_version(before, after) ||
      final_hash.value() != expected.sha256) {
    return Result<PreparedConvolutionIr>::failure(
        {ErrorCode::corrupt_data, "IR source changed during preparation"});
  }
  auto processor = Convolution::create(frames, project_rate, wet);
  if (!processor) {
    return Result<PreparedConvolutionIr>::failure(std::move(processor.error()));
  }
  return Result<PreparedConvolutionIr>::success(
      PreparedConvolutionIr{expected, std::move(processor).value()});
}

} // namespace

Result<PreparedConvolutionIr> prepare_convolution_ir(int fd, const ConvolutionIrIdentity& expected,
                                                     const ConvolutionIrBudget& budget,
                                                     std::uint32_t project_rate, float wet) {
  return prepare_ir(fd, expected, budget.available_preparation_bytes, &budget, project_rate, wet);
}

Result<PreparedConvolutionIr>
prepare_convolution_ir_offline(int fd, const ConvolutionIrIdentity& expected,
                               std::uint64_t available_preparation_bytes,
                               std::uint32_t project_rate, float wet) {
  return prepare_ir(fd, expected, available_preparation_bytes, nullptr, project_rate, wet);
}

} // namespace oscillate::native_dsp
