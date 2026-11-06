#include "oscillate/media/prefetch.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace oscillate::media {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

[[nodiscard]] std::uint16_t le16(const unsigned char* bytes) noexcept {
  return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[0]) |
                                    (static_cast<unsigned>(bytes[1]) << 8U));
}
[[nodiscard]] std::uint32_t le32(const unsigned char* bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}
[[nodiscard]] bool is(const unsigned char* bytes, const char* name) noexcept {
  return std::memcmp(bytes, name, 4) == 0;
}
[[nodiscard]] bool read_exact(int fd, std::uint64_t offset, unsigned char* dst, std::size_t size) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - offset) {
    return false;
  }
  std::size_t done = 0;
  while (done < size) {
    const auto count = ::pread(fd, dst + done, size - done, static_cast<off_t>(offset + done));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    done += static_cast<std::size_t>(count);
  }
  return true;
}
struct WavSampleFormat {
  std::uint16_t code;
  std::uint16_t bits;
};
[[nodiscard]] float sample(const unsigned char* bytes, WavSampleFormat format) noexcept {
  if (format.code == 3) {
    return std::bit_cast<float>(le32(bytes));
  }
  if (format.bits == 8) {
    return (static_cast<float>(bytes[0]) - 128.0F) / 128.0F;
  }
  if (format.bits == 16) {
    return static_cast<float>(static_cast<std::int16_t>(le16(bytes))) / 32768.0F;
  }
  if (format.bits == 24) {
    std::int32_t value = static_cast<std::int32_t>(bytes[0]) |
                         (static_cast<std::int32_t>(bytes[1]) << 8) |
                         (static_cast<std::int32_t>(bytes[2]) << 16);
    if ((value & 0x800000) != 0) {
      value |= static_cast<std::int32_t>(0xFF000000U);
    }
    return static_cast<float>(value) / 8388608.0F;
  }
  return static_cast<float>(std::bit_cast<std::int32_t>(le32(bytes))) / 2147483648.0F;
}

[[nodiscard]] std::size_t checked_capacity(std::size_t capacity) {
  if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("prefetch ring capacity out of range");
  }
  return capacity;
}

} // namespace

WavReader::WavReader(WavReader&& other) noexcept { *this = std::move(other); }

WavReader& WavReader::operator=(WavReader&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
    data_offset_ = other.data_offset_;
    frame_count_ = other.frame_count_;
    sample_rate_ = other.sample_rate_;
    channels_ = other.channels_;
    format_ = other.format_;
    bits_ = other.bits_;
    block_align_ = other.block_align_;
  }
  return *this;
}

WavReader::~WavReader() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Result<WavReader> WavReader::open(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) {
    return Result<WavReader>::failure({ErrorCode::io_failure, "cannot open WAV source"});
  }
  auto result = open_fd(fd);
  ::close(fd);
  return result;
}

Result<WavReader> WavReader::open_fd(int fd) {
  if (fd < 0) {
    return Result<WavReader>::failure({ErrorCode::invalid_argument, "invalid WAV descriptor"});
  }
  const int owned_fd = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
  if (owned_fd < 0) {
    return Result<WavReader>::failure({ErrorCode::io_failure, "cannot duplicate WAV descriptor"});
  }
  WavReader reader;
  reader.fd_ = owned_fd;
  struct stat metadata{};
  if (::fstat(reader.fd_, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
    return Result<WavReader>::failure(
        {ErrorCode::unsupported, "WAV source must be a regular file"});
  }
  const int flags = ::fcntl(reader.fd_, F_GETFL);
  if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY) {
    return Result<WavReader>::failure(
        {ErrorCode::invalid_argument, "WAV descriptor is not readable"});
  }
  if (metadata.st_size < 12) {
    return Result<WavReader>::failure({ErrorCode::corrupt_data, "WAV header is truncated"});
  }
  const auto file_size = static_cast<std::uint64_t>(metadata.st_size);
  std::array<unsigned char, 12> header{};
  if (!read_exact(reader.fd_, 0, header.data(), header.size()) || !is(header.data(), "RIFF") ||
      !is(header.data() + 8, "WAVE")) {
    return Result<WavReader>::failure({ErrorCode::corrupt_data, "invalid RIFF/WAVE header"});
  }
  const std::uint64_t riff_end = 8ULL + le32(header.data() + 4);
  if (riff_end > file_size || riff_end < 12) {
    return Result<WavReader>::failure({ErrorCode::corrupt_data, "RIFF size exceeds file"});
  }
  bool have_format = false;
  bool have_data = false;
  std::uint64_t data_bytes = 0;
  for (std::uint64_t offset = 12; offset + 8 <= riff_end;) {
    std::array<unsigned char, 8> chunk{};
    if (!read_exact(reader.fd_, offset, chunk.data(), chunk.size())) {
      return Result<WavReader>::failure({ErrorCode::corrupt_data, "WAV chunk header truncated"});
    }
    const auto size = static_cast<std::uint64_t>(le32(chunk.data() + 4));
    const auto start = offset + 8;
    if (size > riff_end - start) {
      return Result<WavReader>::failure({ErrorCode::corrupt_data, "WAV chunk exceeds RIFF"});
    }
    if (is(chunk.data(), "fmt ")) {
      if (have_format || size < 16) {
        return Result<WavReader>::failure({ErrorCode::corrupt_data, "invalid WAV format chunk"});
      }
      std::array<unsigned char, 16> fmt{};
      if (!read_exact(reader.fd_, start, fmt.data(), fmt.size())) {
        return Result<WavReader>::failure({ErrorCode::corrupt_data, "WAV format truncated"});
      }
      reader.format_ = le16(fmt.data());
      reader.channels_ = le16(fmt.data() + 2);
      reader.sample_rate_ = le32(fmt.data() + 4);
      const auto byte_rate = le32(fmt.data() + 8);
      reader.block_align_ = le16(fmt.data() + 12);
      reader.bits_ = le16(fmt.data() + 14);
      const bool supported_depth =
          (reader.format_ == 1 &&
           (reader.bits_ == 8 || reader.bits_ == 16 || reader.bits_ == 24 || reader.bits_ == 32)) ||
          (reader.format_ == 3 && reader.bits_ == 32);
      const auto expected_align = static_cast<std::uint32_t>(reader.channels_) *
                                  (static_cast<std::uint32_t>(reader.bits_) / 8U);
      if (!supported_depth || reader.channels_ < 1 || reader.channels_ > 2 ||
          reader.sample_rate_ == 0 || reader.block_align_ != expected_align ||
          static_cast<std::uint64_t>(reader.sample_rate_) * expected_align != byte_rate) {
        return Result<WavReader>::failure({ErrorCode::unsupported, "unsupported WAV format"});
      }
      have_format = true;
    } else if (is(chunk.data(), "data")) {
      if (have_data) {
        return Result<WavReader>::failure({ErrorCode::corrupt_data, "duplicate WAV data chunk"});
      }
      reader.data_offset_ = start;
      data_bytes = size;
      have_data = true;
    }
    const auto next = start + size + (size & 1U);
    if (next > riff_end) {
      return Result<WavReader>::failure({ErrorCode::corrupt_data, "WAV padding exceeds RIFF"});
    }
    offset = next;
  }
  if (!have_format || !have_data || data_bytes % reader.block_align_ != 0) {
    return Result<WavReader>::failure(
        {ErrorCode::corrupt_data, "WAV format/data missing or misaligned"});
  }
  reader.frame_count_ = data_bytes / reader.block_align_;
  return Result<WavReader>::success(std::move(reader));
}

Result<std::size_t> WavReader::read_at(std::uint64_t source_frame, std::span<StereoFrame> output) {
  if (source_frame >= frame_count_ || output.empty()) {
    return Result<std::size_t>::success(0);
  }
  const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), frame_count_ - source_frame));
  const auto offset = data_offset_ + source_frame * block_align_;
  std::array<unsigned char, 8192> bytes{};
  for (std::size_t base = 0; base < count;) {
    const auto frames = std::min(count - base, bytes.size() / block_align_);
    if (!read_exact(fd_, offset + base * block_align_, bytes.data(), frames * block_align_)) {
      return Result<std::size_t>::failure(
          {ErrorCode::io_failure, "WAV source changed during read"});
    }
    const WavSampleFormat format{format_, bits_};
    for (std::size_t index = 0; index < frames; ++index) {
      const auto* frame = bytes.data() + index * block_align_;
      const auto left = sample(frame, format);
      const auto right = channels_ == 2 ? sample(frame + bits_ / 8, format) : left;
      if (!std::isfinite(left) || !std::isfinite(right)) {
        return Result<std::size_t>::failure({ErrorCode::corrupt_data, "non-finite WAV sample"});
      }
      output[base + index] = {left, right};
    }
    base += frames;
  }
  return Result<std::size_t>::success(count);
}

PrefetchRing::PrefetchRing(ClipId clip, std::size_t capacity)
    : clip_(clip), frames_(checked_capacity(capacity), TaggedFrame{clip, 0, 0, {}}) {}

void PrefetchRing::push_underrun(UnderrunEvent event) noexcept {
  const auto write = underrun_event_write_.load(std::memory_order_relaxed);
  if (write - underrun_event_read_.load(std::memory_order_acquire) >= underrun_event_capacity) {
    const auto dropped = dropped_underrun_events_.load(std::memory_order_relaxed);
    if (dropped != std::numeric_limits<std::uint64_t>::max())
      dropped_underrun_events_.store(dropped + 1, std::memory_order_relaxed);
    return;
  }
  underrun_events_[static_cast<std::size_t>(write % underrun_event_capacity)] = event;
  underrun_event_write_.store(write + 1, std::memory_order_release);
}

std::optional<UnderrunEvent> PrefetchRing::pop_underrun() noexcept {
  const auto read = underrun_event_read_.load(std::memory_order_relaxed);
  if (read == underrun_event_write_.load(std::memory_order_acquire))
    return std::nullopt;
  const auto event = underrun_events_[static_cast<std::size_t>(read % underrun_event_capacity)];
  underrun_event_read_.store(read + 1, std::memory_order_release);
  return event;
}

std::size_t PrefetchRing::occupancy() const noexcept {
  const auto read = read_.load(std::memory_order_acquire);
  const auto written = write_.load(std::memory_order_acquire);
  // Independent atomic observations can straddle many push/pop operations.
  // Clamp either skew so monitoring and producer free-space arithmetic remain
  // bounded; an underestimated free space merely delays prefetch.
  if (written < read) {
    return 0;
  }
  return static_cast<std::size_t>(std::min<std::uint64_t>(written - read, frames_.size()));
}

bool PrefetchRing::try_push(const TaggedFrame& frame) noexcept {
  if (frame.clip != clip_) {
    return false;
  }
  if (have_last_tag_ && (frame.generation < last_generation_ ||
                         (frame.generation == last_generation_ &&
                          (last_source_frame_ == std::numeric_limits<std::uint64_t>::max() ||
                           frame.source_frame != last_source_frame_ + 1)))) {
    return false;
  }
  const auto write = write_.load(std::memory_order_relaxed);
  if (write - read_.load(std::memory_order_acquire) >= frames_.size()) {
    return false;
  }
  frames_[static_cast<std::size_t>(write % frames_.size())] = frame;
  write_.store(write + 1, std::memory_order_release);
  have_last_tag_ = true;
  last_generation_ = frame.generation;
  last_source_frame_ = frame.source_frame;
  return true;
}

bool PrefetchRing::skip_forward(std::uint64_t generation, std::uint64_t next_frame) noexcept {
  if (!have_last_tag_ || generation != last_generation_ ||
      last_source_frame_ == std::numeric_limits<std::uint64_t>::max() ||
      next_frame <= last_source_frame_ + 1)
    return false;
  last_source_frame_ = next_frame - 1;
  return true;
}

HandoffWindowStatus PrefetchRing::prime_handoff_window(std::uint64_t generation,
                                                       std::uint64_t source_frame,
                                                       std::size_t frames) noexcept {
  if (generation == 0 || frames == 0 || frames > capacity() ||
      frames - 1 > std::numeric_limits<std::uint64_t>::max() - source_frame)
    return HandoffWindowStatus::invalid;
  auto read = read_.load(std::memory_order_relaxed);
  const auto write = write_.load(std::memory_order_acquire);
  for (auto cursor = read; cursor < write; ++cursor) {
    const auto& head = frames_[static_cast<std::size_t>(cursor % capacity())];
    if (head.generation < generation ||
        (head.generation == generation && head.source_frame < source_frame))
      continue;
    if (head.generation != generation || head.source_frame != source_frame) {
      read_.store(cursor, std::memory_order_release);
      return HandoffWindowStatus::missed;
    }
    read_.store(cursor, std::memory_order_release);
    if (write - cursor < frames)
      return HandoffWindowStatus::waiting;
    for (std::size_t offset = 0; offset < frames; ++offset) {
      const auto& tagged = frames_[static_cast<std::size_t>((cursor + offset) % capacity())];
      if (tagged.generation != generation || tagged.source_frame != source_frame + offset)
        return HandoffWindowStatus::missed;
    }
    return HandoffWindowStatus::ready;
  }
  // The producer can immediately reuse the discarded prefix after this
  // release; the scan no longer touches those slots.
  read_.store(write, std::memory_order_release);
  return HandoffWindowStatus::waiting;
}

ConsumeResult PrefetchRing::consume(std::uint64_t generation, std::uint64_t source_frame,
                                    std::span<StereoFrame> output, bool steady) noexcept {
  ConsumeResult result;
  std::ranges::fill(output, StereoFrame{});
  std::size_t event_index = 0;
  const auto add_gap = [&](std::size_t start, std::size_t count) noexcept {
    result.silent += count;
    if (steady && count != 0) {
      const auto event = UnderrunEvent{clip_, generation, source_frame + start, count};
      push_underrun(event);
      if (event_index < result.underruns.size())
        result.underruns[event_index++] = event;
      else
        ++result.unlisted_underruns;
      const auto previous = underrun_count_.load(std::memory_order_relaxed);
      if (previous != std::numeric_limits<std::uint64_t>::max()) {
        underrun_count_.store(previous + 1, std::memory_order_relaxed);
      }
    }
  };
  if (output.size() > std::numeric_limits<std::uint64_t>::max() - source_frame) {
    // Invalid near-overflow request: silence without forming an overflowing tag.
    result.silent = output.size();
    return result;
  }
  consumed_end_.store(source_frame + output.size(), std::memory_order_release);
  consumed_generation_.store(generation, std::memory_order_release);
  auto read = read_.load(std::memory_order_relaxed);
  const auto stale_limit = max_stale_scan_per_block;
  std::size_t index = 0;
  while (index < output.size() && result.stale_dropped < stale_limit) {
    const auto write = write_.load(std::memory_order_acquire);
    if (read == write) {
      add_gap(index, output.size() - index);
      break;
    }
    const auto& head = frames_[static_cast<std::size_t>(read % frames_.size())];
    const auto expected = source_frame + index;
    if (head.generation < generation ||
        (head.generation == generation && head.source_frame < expected)) {
      ++read;
      ++result.stale_dropped;
      read_.store(read, std::memory_order_release);
      continue;
    }
    if (head.generation > generation) {
      add_gap(index, output.size() - index);
      break;
    }
    if (head.source_frame > expected) {
      const auto gap = static_cast<std::size_t>(
          std::min<std::uint64_t>(head.source_frame - expected, output.size() - index));
      add_gap(index, gap);
      index += gap;
      continue;
    }
    output[index++] = head.audio;
    ++result.copied;
    ++read;
    read_.store(read, std::memory_order_release);
  }
  if (index < output.size() && result.silent < output.size() - result.copied) {
    // Reached stale scan bound; keep callback work finite.
    add_gap(index, output.size() - index);
  }
  return result;
}

void PrefetchRing::discard_pending() noexcept {
  read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
}

Result<std::size_t> fill_from_wav(WavReader& source, std::uint32_t project_rate, PrefetchRing& ring,
                                  PrefetchCursor cursor, std::span<StereoFrame> scratch) {
  if (source.sample_rate() != project_rate) {
    return Result<std::size_t>::failure(
        {ErrorCode::unsupported, "WAV source rate differs from project"});
  }
  if (cursor.source_frame > std::numeric_limits<std::uint64_t>::max() - scratch.size()) {
    return Result<std::size_t>::failure({ErrorCode::invalid_argument, "source frame overflow"});
  }
  const auto budget = std::min(scratch.size(), ring.free_slots());
  auto decoded = source.read_at(cursor.source_frame, scratch.first(budget));
  if (!decoded) {
    return Result<std::size_t>::failure(decoded.error());
  }
  for (std::size_t i = 0; i < decoded.value(); ++i) {
    if (!ring.try_push(
            TaggedFrame{ring.clip(), cursor.generation, cursor.source_frame + i, scratch[i]})) {
      return Result<std::size_t>::failure(
          {ErrorCode::internal, "ring occupancy changed unexpectedly"});
    }
  }
  return Result<std::size_t>::success(decoded.value());
}

} // namespace oscillate::media
