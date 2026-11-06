#include <oscillate/source_prep/managed_audio.hpp>

#include <oscillate/foundation/file_io.hpp>
#include <oscillate/media/audio_reader.hpp>
#include <oscillate/persistence/store.hpp>
#include <oscillate/project/model.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <unistd.h>
#include <utility>
#include <vector>

namespace oscillate::source_prep {
namespace {
using foundation::ErrorCode;
using foundation::Result;

} // namespace

Result<DecodedAudio> decode_audio_fd(int fd, const project::SourceRecord& record,
                                     std::size_t max_frames, const std::stop_token& stop) {
  if (max_frames == 0 || max_frames > max_managed_audio_frames) {
    return Result<DecodedAudio>::failure(
        {ErrorCode::invalid_argument, "managed audio frame budget is outside supported range"});
  }
  if (record.frames == 0) {
    return Result<DecodedAudio>::failure({ErrorCode::corrupt_data, "managed audio has no frames"});
  }
  if (record.frames > max_frames) {
    return Result<DecodedAudio>::failure(
        {ErrorCode::resource_exhausted, "managed audio exceeds preparation frame budget"});
  }
  auto decoded = media::AudioReader::open_fd(fd, record);
  if (!decoded) {
    return Result<DecodedAudio>::failure(decoded.error());
  }
  auto reader = std::move(decoded).value();

  DecodedAudio result;
  result.source_rate = reader.sample_rate();
  try {
    result.frames.resize(static_cast<std::size_t>(record.frames));
  } catch (const std::bad_alloc&) {
    return Result<DecodedAudio>::failure(
        {ErrorCode::resource_exhausted, "cannot allocate decoded managed audio"});
  }
  std::array<media::StereoFrame, 1024> scratch{};
  for (std::size_t base = 0; base < result.frames.size();) {
    if (stop.stop_requested())
      return Result<DecodedAudio>::failure(
          {ErrorCode::cancelled, "managed audio decode cancelled"});
    const auto count = std::min(scratch.size(), result.frames.size() - base);
    auto read = reader.read_at(base, std::span(scratch).first(count));
    if (!read) {
      return Result<DecodedAudio>::failure(read.error());
    }
    if (read.value() != count) {
      return Result<DecodedAudio>::failure(
          {ErrorCode::corrupt_data, "managed audio ended before declared frame count"});
    }
    for (std::size_t index = 0; index < count; ++index) {
      const auto frame = scratch[index];
      result.frames[base + index] = {frame.left, frame.right};
    }
    base += count;
  }
  return Result<DecodedAudio>::success(std::move(result));
}

Result<DecodedAudio> load_managed_audio(const persistence::ProjectStore& store,
                                        const project::SourceRecord& record, std::size_t max_frames,
                                        const std::stop_token& stop) {
  if (max_frames == 0 || max_frames > max_managed_audio_frames) {
    return Result<DecodedAudio>::failure(
        {ErrorCode::invalid_argument, "managed audio frame budget is outside supported range"});
  }
  if (record.frames == 0) {
    return Result<DecodedAudio>::failure({ErrorCode::corrupt_data, "managed audio has no frames"});
  }
  if (record.frames > max_frames) {
    return Result<DecodedAudio>::failure(
        {ErrorCode::resource_exhausted, "managed audio exceeds preparation frame budget"});
  }
  auto opened = store.open_verified_source(record, stop);
  if (!opened) {
    return Result<DecodedAudio>::failure(opened.error());
  }
  const foundation::UniqueFd pinned(opened.value());
  return decode_audio_fd(opened.value(), record, max_frames, stop);
}

} // namespace oscillate::source_prep
