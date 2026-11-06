#include "oscillate/media/audio_reader.hpp"

#if OSCILLATE_HAVE_CODEC
#include "oscillate/codec/encoded_reader.hpp"
#endif

#include <algorithm>
#include <limits>
#include <utility>
#include <variant>

namespace oscillate::media {
namespace {
using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

#if OSCILLATE_HAVE_CODEC
[[nodiscard]] bool matches(project::SourceEncoding encoding, codec::EncodedFormat format) {
  switch (encoding) {
  case project::SourceEncoding::wav:
    return format == codec::EncodedFormat::wav;
  case project::SourceEncoding::aiff:
    return format == codec::EncodedFormat::aiff;
  case project::SourceEncoding::flac:
    return format == codec::EncodedFormat::flac;
  case project::SourceEncoding::mp3:
    return format == codec::EncodedFormat::mp3;
  }
  return false;
}
#endif
} // namespace

struct AudioReader::Backend {
  explicit Backend(WavReader source) : reader(std::move(source)) {}
#if OSCILLATE_HAVE_CODEC
  explicit Backend(codec::EncodedReader source) : reader(std::move(source)) {}
  std::variant<WavReader, codec::EncodedReader> reader;
  std::array<codec::StereoFrame, 4096> conversion{};
#else
  WavReader reader;
#endif
};

AudioReader::AudioReader(std::unique_ptr<Backend> backend) noexcept
    : backend_(std::move(backend)) {}
AudioReader::AudioReader(AudioReader&&) noexcept = default;
AudioReader& AudioReader::operator=(AudioReader&&) noexcept = default;
AudioReader::~AudioReader() = default;

AudioReader AudioReader::from_wav(WavReader reader) {
  return AudioReader(std::make_unique<Backend>(std::move(reader)));
}

Result<AudioReader> AudioReader::open_fd(int fd, const project::SourceRecord& record) {
  if (record.encoding == project::SourceEncoding::wav) {
    auto opened = WavReader::open_fd(fd);
    if (!opened) {
      return Result<AudioReader>::failure(opened.error());
    }
    if (opened.value().sample_rate() != record.sample_rate ||
        opened.value().channels() != record.channels ||
        opened.value().frame_count() != record.frames) {
      return Result<AudioReader>::failure(
          {ErrorCode::corrupt_data, "Managed WAV metadata differs from project record"});
    }
    return Result<AudioReader>::success(from_wav(std::move(opened).value()));
  }
#if OSCILLATE_HAVE_CODEC
  auto opened = codec::EncodedReader::open_fd(fd);
  if (!opened) {
    return Result<AudioReader>::failure(opened.error());
  }
  const auto& info = opened.value().info();
  if (!matches(record.encoding, info.format) || info.sample_rate != record.sample_rate ||
      info.channels != record.channels || info.frames != record.frames ||
      info.encoded_bytes != record.byte_length) {
    return Result<AudioReader>::failure(
        {ErrorCode::corrupt_data, "Managed audio metadata differs from project record"});
  }
  return Result<AudioReader>::success(
      AudioReader(std::make_unique<Backend>(std::move(opened).value())));
#else
  (void)fd;
  return Result<AudioReader>::failure(
      {ErrorCode::unsupported, "Encoded audio playback requires the codec adapter"});
#endif
}

Result<std::size_t> AudioReader::read_at(std::uint64_t source_frame,
                                         std::span<StereoFrame> output) {
#if OSCILLATE_HAVE_CODEC
  if (auto* wav = std::get_if<WavReader>(&backend_->reader)) {
    return wav->read_at(source_frame, output);
  }
  auto& encoded = std::get<codec::EncodedReader>(backend_->reader);
  std::size_t decoded = 0;
  while (decoded < output.size()) {
    const auto wanted = std::min(output.size() - decoded, backend_->conversion.size());
    auto got = encoded.read_at(source_frame + decoded,
                               std::span<codec::StereoFrame>(backend_->conversion).first(wanted));
    if (!got) {
      return Result<std::size_t>::failure(got.error());
    }
    for (std::size_t index = 0; index < got.value(); ++index) {
      const auto& frame = backend_->conversion[index];
      output[decoded + index] = {frame.left, frame.right};
    }
    decoded += got.value();
    if (got.value() < wanted) {
      break;
    }
  }
  return Result<std::size_t>::success(decoded);
#else
  return backend_->reader.read_at(source_frame, output);
#endif
}

std::uint32_t AudioReader::sample_rate() const noexcept {
#if OSCILLATE_HAVE_CODEC
  if (const auto* wav = std::get_if<WavReader>(&backend_->reader)) {
    return wav->sample_rate();
  }
  const auto* encoded = std::get_if<codec::EncodedReader>(&backend_->reader);
  return encoded == nullptr ? 0 : encoded->info().sample_rate;
#else
  return backend_->reader.sample_rate();
#endif
}

std::uint32_t AudioReader::channels() const noexcept {
#if OSCILLATE_HAVE_CODEC
  if (const auto* wav = std::get_if<WavReader>(&backend_->reader)) {
    return wav->channels();
  }
  const auto* encoded = std::get_if<codec::EncodedReader>(&backend_->reader);
  return encoded == nullptr ? 0 : encoded->info().channels;
#else
  return backend_->reader.channels();
#endif
}

std::uint64_t AudioReader::frame_count() const noexcept {
#if OSCILLATE_HAVE_CODEC
  if (const auto* wav = std::get_if<WavReader>(&backend_->reader)) {
    return wav->frame_count();
  }
  const auto* encoded = std::get_if<codec::EncodedReader>(&backend_->reader);
  return encoded == nullptr ? 0 : encoded->info().frames;
#else
  return backend_->reader.frame_count();
#endif
}

Result<std::size_t> fill_from_audio(AudioReader& source, std::uint32_t project_rate,
                                    PrefetchRing& ring, PrefetchCursor cursor,
                                    std::span<StereoFrame> scratch) {
  if (source.sample_rate() != project_rate) {
    return Result<std::size_t>::failure(
        {ErrorCode::unsupported, "Audio source rate differs from project"});
  }
  if (cursor.source_frame > std::numeric_limits<std::uint64_t>::max() - scratch.size()) {
    return Result<std::size_t>::failure({ErrorCode::invalid_argument, "source frame overflow"});
  }
  const auto budget = std::min(scratch.size(), ring.free_slots());
  auto decoded = source.read_at(cursor.source_frame, scratch.first(budget));
  if (!decoded) {
    return Result<std::size_t>::failure(decoded.error());
  }
  for (std::size_t index = 0; index < decoded.value(); ++index) {
    if (!ring.try_push(
            {ring.clip(), cursor.generation, cursor.source_frame + index, scratch[index]})) {
      return Result<std::size_t>::failure(
          {ErrorCode::internal, "ring occupancy changed unexpectedly"});
    }
  }
  return Result<std::size_t>::success(decoded.value());
}

} // namespace oscillate::media
