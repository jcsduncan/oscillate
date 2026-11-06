#include "oscillate/analysis/cache_payload.hpp"
#include "oscillate/foundation/midi.hpp"
#include "oscillate/timeline/timing.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace oscillate::analysis {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr std::array<std::byte, 8> magic{std::byte{'L'}, std::byte{'D'}, std::byte{'A'},
                                         std::byte{'N'}, std::byte{'L'}, std::byte{'Y'},
                                         std::byte{'S'}, std::byte{'1'}};
constexpr std::size_t max_payload_bytes = std::size_t{64} * 1024U * 1024U;
constexpr std::size_t max_key_bytes = 128;
constexpr std::uint32_t max_waveform_buckets = 1'000'000;
constexpr std::uint32_t max_waveform_levels = 16;
constexpr std::uint32_t max_tempo_suggestions = 50'000;
constexpr std::uint64_t fnv_offset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t fnv_prime = 1'099'511'628'211ULL;
constexpr std::uint8_t waveform_kind = 1;
constexpr std::uint8_t tempo_kind = 2;
constexpr std::uint8_t key_kind = 3;

template <typename UInt> void append(std::vector<std::byte>& output, UInt value) {
  static_assert(std::is_unsigned_v<UInt>);
  std::uint64_t remaining = value;
  for (std::size_t byte = 0; byte < sizeof(UInt); ++byte) {
    output.push_back(static_cast<std::byte>(remaining & 0xffU));
    remaining >>= 8U;
  }
}

void append_double(std::vector<std::byte>& output, double value) {
  append(output, std::bit_cast<std::uint64_t>(value));
}

void append_float(std::vector<std::byte>& output, float value) {
  append(output, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] std::uint64_t checksum(std::span<const std::byte> bytes) {
  std::uint64_t value = fnv_offset;
  for (const auto byte : bytes) {
    value ^= std::to_integer<std::uint8_t>(byte);
    value *= fnv_prime;
  }
  return value;
}

class Cursor {
public:
  explicit Cursor(std::span<const std::byte> bytes) : bytes_(bytes) {}

  template <typename UInt> [[nodiscard]] bool read(UInt& value) {
    static_assert(std::is_unsigned_v<UInt>);
    if (remaining() < sizeof(UInt)) {
      return false;
    }
    value = 0;
    for (std::size_t byte = 0; byte < sizeof(UInt); ++byte) {
      value = static_cast<UInt>(
          value | (static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[position_++]))
                   << (8U * byte)));
    }
    return true;
  }

  [[nodiscard]] bool take(std::size_t count, std::span<const std::byte>& value) {
    if (count > remaining()) {
      return false;
    }
    value = bytes_.subspan(position_, count);
    position_ += count;
    return true;
  }

  [[nodiscard]] std::size_t remaining() const { return bytes_.size() - position_; }

private:
  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
};

[[nodiscard]] bool valid_source(SourceMetadata source) {
  return source.sample_rate != 0 && (source.channels == 1 || source.channels == 2);
}

[[nodiscard]] bool valid_key(std::string_view key, std::uint64_t generation) {
  return !key.empty() && key.size() <= max_key_bytes && generation != 0;
}

[[nodiscard]] bool valid_channel(const ChannelSummary& value) {
  return std::isfinite(value.minimum) && std::isfinite(value.maximum) && std::isfinite(value.rms) &&
         value.minimum <= value.maximum && value.rms >= 0.0;
}

[[nodiscard]] bool valid_waveform(const WaveformSummary& summary) {
  if (!valid_key(summary.identity.source_key, summary.identity.generation) ||
      !valid_source({summary.sample_rate, summary.channels, summary.frames}) ||
      summary.levels.size() > max_waveform_levels ||
      (summary.frames == 0) != summary.levels.empty()) {
    return false;
  }
  std::uint32_t total = 0;
  std::uint64_t previous_width = 0;
  for (const auto& level : summary.levels) {
    const auto width = level.frames_per_bucket;
    if (width == 0 || width > summary.frames || width <= previous_width ||
        level.buckets.size() > max_waveform_buckets - total ||
        level.buckets.size() !=
            summary.frames / width + static_cast<std::uint64_t>(summary.frames % width != 0)) {
      return false;
    }
    total += static_cast<std::uint32_t>(level.buckets.size());
    previous_width = width;
    std::uint64_t begin = 0;
    for (const auto& bucket : level.buckets) {
      const auto expected_end = begin + std::min(width, summary.frames - begin);
      if (bucket.begin_frame != begin || bucket.end_frame != expected_end) {
        return false;
      }
      for (std::uint32_t channel = 0; channel < summary.channels; ++channel) {
        if (!valid_channel(bucket.channels[channel])) {
          return false;
        }
      }
      begin = expected_end;
    }
  }
  return true;
}

[[nodiscard]] bool valid_tempo(const TempoEstimate& estimate, std::uint32_t channels) {
  if (!valid_key(estimate.identity.source_key, estimate.identity.generation) ||
      !valid_source({estimate.sample_rate, channels, estimate.frames}) ||
      !std::isfinite(estimate.bpm) || !std::isfinite(estimate.confidence) ||
      estimate.confidence < 0.0 || estimate.confidence > 1.0 ||
      estimate.suggested_beat_frames.size() > max_tempo_suggestions ||
      estimate.suggested_onset_frames.size() > max_tempo_suggestions ||
      (estimate.suggested_onset_frames.empty() && estimate.onsets_truncated) ||
      (estimate.bpm == 0.0 &&
       (estimate.confidence != 0.0 || !estimate.suggested_beat_frames.empty() ||
        estimate.suggestions_truncated)) ||
      (estimate.bpm != 0.0 &&
       (estimate.bpm < timeline::min_detected_bpm || estimate.bpm > timeline::max_detected_bpm))) {
    return false;
  }
  std::uint64_t previous = 0;
  bool first = true;
  for (const auto frame : estimate.suggested_beat_frames) {
    if (frame >= estimate.frames || (!first && frame <= previous)) {
      return false;
    }
    first = false;
    previous = frame;
  }
  previous = 0;
  first = true;
  for (const auto frame : estimate.suggested_onset_frames) {
    if (frame >= estimate.frames || (!first && frame <= previous)) {
      return false;
    }
    first = false;
    previous = frame;
  }
  return true;
}

[[nodiscard]] bool valid_musical_key(MusicalKey key) {
  return key.tonic < foundation::semitones_per_octave &&
         (key.mode == KeyMode::major || key.mode == KeyMode::minor);
}

[[nodiscard]] MusicalKey expected_compatible(MusicalKey key, KeyRelation relation) {
  switch (relation) {
  case KeyRelation::relative:
    return {static_cast<std::uint8_t>((key.tonic + (key.mode == KeyMode::major ? 9 : 3)) %
                                      foundation::semitones_per_octave),
            key.mode == KeyMode::major ? KeyMode::minor : KeyMode::major};
  case KeyRelation::dominant:
    return {static_cast<std::uint8_t>((key.tonic + 7) % foundation::semitones_per_octave),
            key.mode};
  case KeyRelation::subdominant:
    return {static_cast<std::uint8_t>((key.tonic + 5) % foundation::semitones_per_octave),
            key.mode};
  }
  return {};
}

[[nodiscard]] bool valid_key_estimate(const KeyEstimate& estimate, std::uint32_t channels) {
  if (!valid_key(estimate.identity.source_key, estimate.identity.generation) ||
      !valid_source({estimate.sample_rate, channels, estimate.frames}) ||
      !std::isfinite(estimate.confidence) || estimate.confidence < 0.0 ||
      estimate.confidence > 1.0) {
    return false;
  }
  if (!estimate.key) {
    return estimate.confidence == 0.0 && estimate.compatible_keys.empty();
  }
  if (!valid_musical_key(*estimate.key) || estimate.confidence <= 0.0 ||
      estimate.compatible_keys.size() != 3) {
    return false;
  }
  constexpr std::array relations{KeyRelation::relative, KeyRelation::dominant,
                                 KeyRelation::subdominant};
  for (std::size_t index = 0; index < relations.size(); ++index) {
    const auto& compatible = estimate.compatible_keys[index];
    if (compatible.relation != relations[index] || !valid_musical_key(compatible.key) ||
        !(compatible.key.tonic == expected_compatible(*estimate.key, compatible.relation).tonic &&
          compatible.key.mode == expected_compatible(*estimate.key, compatible.relation).mode)) {
      return false;
    }
  }
  return true;
}

void append_header(std::vector<std::byte>& output, std::uint8_t kind, std::string_view key,
                   SourceMetadata source, std::uint16_t version = analysis_cache_payload_version) {
  output.insert(output.end(), magic.begin(), magic.end());
  append(output, version);
  append(output, kind);
  append(output, std::uint8_t{0});
  append(output, std::uint32_t{0}); // total length filled after the body is complete
  append(output, static_cast<std::uint16_t>(key.size()));
  append(output, std::uint16_t{0});
  // Job generation is transient and must never invalidate a reusable cache.
  append(output, std::uint64_t{0});
  append(output, source.sample_rate);
  append(output, source.channels);
  append(output, source.frames);
  for (const auto character : key) {
    output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
}

void finish(std::vector<std::byte>& output) {
  // Header bytes 12..15 hold the exact total payload length, including digest.
  const auto length = static_cast<std::uint32_t>(output.size() + sizeof(std::uint64_t));
  for (std::size_t byte = 0; byte < sizeof(length); ++byte) {
    output[12 + byte] = static_cast<std::byte>((length >> (8U * byte)) & 0xffU);
  }
  append(output, checksum(output));
}

template <typename Identity>
[[nodiscard]] Result<Cursor> open(std::span<const std::byte> bytes, std::uint8_t kind,
                                  const Identity& expected, SourceMetadata source,
                                  std::uint16_t expected_version = analysis_cache_payload_version) {
  const auto corrupt = [] {
    return Result<Cursor>::failure({ErrorCode::corrupt_data, "Invalid analysis cache payload"});
  };
  if (bytes.size() < 52 || bytes.size() > max_payload_bytes ||
      !std::equal(magic.begin(), magic.end(), bytes.begin())) {
    return corrupt();
  }
  // The inner checksum catches accidental mutation even when this codec is
  // used without CacheStore's SHA-256-protected file envelope.
  Cursor checksum_cursor(bytes.last(sizeof(std::uint64_t)));
  std::uint64_t saved_checksum = 0;
  if (!checksum_cursor.read(saved_checksum) ||
      saved_checksum != checksum(bytes.first(bytes.size() - sizeof(std::uint64_t)))) {
    return corrupt();
  }
  Cursor cursor(bytes.first(bytes.size() - sizeof(std::uint64_t)));
  std::span<const std::byte> read_magic;
  std::uint16_t version = 0;
  std::uint8_t stored_kind = 0;
  std::uint8_t reserved = 0;
  std::uint32_t total_bytes = 0;
  std::uint16_t key_length = 0;
  std::uint16_t reserved2 = 0;
  std::uint64_t reserved_generation = 0;
  SourceMetadata stored;
  if (!cursor.take(magic.size(), read_magic) || !cursor.read(version) ||
      !cursor.read(stored_kind) || !cursor.read(reserved) || !cursor.read(total_bytes) ||
      !cursor.read(key_length) || !cursor.read(reserved2) || !cursor.read(reserved_generation) ||
      !cursor.read(stored.sample_rate) || !cursor.read(stored.channels) ||
      !cursor.read(stored.frames) || version != expected_version || stored_kind != kind ||
      reserved != 0 || reserved2 != 0 || total_bytes != bytes.size() || key_length == 0 ||
      key_length > max_key_bytes || reserved_generation != 0 || !valid_source(stored)) {
    return corrupt();
  }
  std::span<const std::byte> key;
  if (!cursor.take(key_length, key)) {
    return corrupt();
  }
  if (!valid_key(expected.source_key, expected.generation) ||
      expected.source_key.size() != key.size() || !valid_source(source) ||
      source.sample_rate != stored.sample_rate || source.channels != stored.channels ||
      source.frames != stored.frames ||
      !std::equal(
          key.begin(), key.end(), expected.source_key.begin(), [](std::byte left, char right) {
            return std::to_integer<unsigned char>(left) == static_cast<unsigned char>(right);
          })) {
    return Result<Cursor>::failure(
        {ErrorCode::conflict, "Stale analysis cache identity or source"});
  }
  return Result<Cursor>::success(cursor);
}

} // namespace

Result<std::vector<std::byte>> encode_waveform_cache(const WaveformSummary& summary) {
  if (!valid_waveform(summary)) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::invalid_argument, "Invalid waveform cache result"});
  }
  std::uint64_t bytes = 44 + summary.identity.source_key.size() + 4 + 8;
  for (const auto& level : summary.levels) {
    bytes += 12 + static_cast<std::uint64_t>(level.buckets.size()) * (16 + summary.channels * 16);
  }
  if (bytes > max_payload_bytes) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::resource_exhausted, "Waveform cache exceeds payload budget"});
  }
  std::vector<std::byte> output;
  output.reserve(static_cast<std::size_t>(bytes));
  append_header(output, waveform_kind, summary.identity.source_key,
                {summary.sample_rate, summary.channels, summary.frames});
  append(output, static_cast<std::uint32_t>(summary.levels.size()));
  for (const auto& level : summary.levels) {
    append(output, level.frames_per_bucket);
    append(output, static_cast<std::uint32_t>(level.buckets.size()));
    for (const auto& bucket : level.buckets) {
      append(output, bucket.begin_frame);
      append(output, bucket.end_frame);
      for (std::uint32_t channel = 0; channel < summary.channels; ++channel) {
        const auto& value = bucket.channels[channel];
        append_float(output, value.minimum);
        append_float(output, value.maximum);
        append_double(output, value.rms);
      }
    }
  }
  finish(output);
  return Result<std::vector<std::byte>>::success(std::move(output));
}

Result<WaveformSummary> decode_waveform_cache(std::span<const std::byte> bytes,
                                              const WaveformIdentity& expected,
                                              SourceMetadata source) {
  auto opened = open(bytes, waveform_kind, expected, source);
  if (!opened) {
    return Result<WaveformSummary>::failure(opened.error());
  }
  auto cursor = std::move(opened).value();
  const auto corrupt = [] {
    return Result<WaveformSummary>::failure(
        {ErrorCode::corrupt_data, "Invalid waveform cache levels"});
  };
  std::uint32_t level_count = 0;
  if (!cursor.read(level_count) || level_count > max_waveform_levels ||
      (source.frames == 0) != (level_count == 0)) {
    return corrupt();
  }
  WaveformSummary result{.identity = expected,
                         .sample_rate = source.sample_rate,
                         .channels = source.channels,
                         .frames = source.frames,
                         .levels = {}};
  result.levels.reserve(level_count);
  std::uint32_t total_buckets = 0;
  std::uint64_t previous_width = 0;
  for (std::uint32_t level = 0; level < level_count; ++level) {
    std::uint64_t width = 0;
    std::uint32_t count = 0;
    if (!cursor.read(width) || !cursor.read(count) || width == 0 || width > source.frames ||
        width <= previous_width || count > max_waveform_buckets - total_buckets ||
        count != source.frames / width + static_cast<std::uint64_t>(source.frames % width != 0) ||
        count > cursor.remaining() / (16 + source.channels * 16)) {
      return corrupt();
    }
    previous_width = width;
    total_buckets += count;
    WaveformLevel current{.frames_per_bucket = width, .buckets = {}};
    current.buckets.reserve(count);
    std::uint64_t begin = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
      WaveformBucket bucket;
      if (!cursor.read(bucket.begin_frame) || !cursor.read(bucket.end_frame) ||
          bucket.begin_frame != begin ||
          bucket.end_frame != begin + std::min(width, source.frames - begin)) {
        return corrupt();
      }
      for (std::uint32_t channel = 0; channel < source.channels; ++channel) {
        std::uint32_t minimum = 0;
        std::uint32_t maximum = 0;
        std::uint64_t rms = 0;
        if (!cursor.read(minimum) || !cursor.read(maximum) || !cursor.read(rms)) {
          return corrupt();
        }
        bucket.channels[channel] = {.minimum = std::bit_cast<float>(minimum),
                                    .maximum = std::bit_cast<float>(maximum),
                                    .rms = std::bit_cast<double>(rms)};
        if (!valid_channel(bucket.channels[channel])) {
          return corrupt();
        }
      }
      begin = bucket.end_frame;
      current.buckets.push_back(bucket);
    }
    result.levels.push_back(std::move(current));
  }
  if (cursor.remaining() != 0) {
    return corrupt();
  }
  return Result<WaveformSummary>::success(std::move(result));
}

Result<std::vector<std::byte>> encode_tempo_cache(const TempoEstimate& estimate,
                                                  std::uint32_t source_channels) {
  if (!valid_tempo(estimate, source_channels)) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::invalid_argument, "Invalid tempo cache result"});
  }
  const auto bytes = 44 + estimate.identity.source_key.size() + 8 + 8 + 1 + 4 +
                     estimate.suggested_beat_frames.size() * 8 + 1 + 4 +
                     estimate.suggested_onset_frames.size() * 8 + 8;
  std::vector<std::byte> output;
  output.reserve(bytes);
  append_header(output, tempo_kind, estimate.identity.source_key,
                {estimate.sample_rate, source_channels, estimate.frames},
                tempo_cache_payload_version);
  append_double(output, estimate.bpm);
  append_double(output, estimate.confidence);
  append(output, static_cast<std::uint8_t>(estimate.suggestions_truncated));
  append(output, static_cast<std::uint32_t>(estimate.suggested_beat_frames.size()));
  for (const auto frame : estimate.suggested_beat_frames) {
    append(output, frame);
  }
  append(output, static_cast<std::uint8_t>(estimate.onsets_truncated));
  append(output, static_cast<std::uint32_t>(estimate.suggested_onset_frames.size()));
  for (const auto frame : estimate.suggested_onset_frames) {
    append(output, frame);
  }
  finish(output);
  return Result<std::vector<std::byte>>::success(std::move(output));
}

Result<TempoEstimate> decode_tempo_cache(std::span<const std::byte> bytes,
                                         const TempoIdentity& expected, SourceMetadata source) {
  auto opened = open(bytes, tempo_kind, expected, source, tempo_cache_payload_version);
  if (!opened) {
    return Result<TempoEstimate>::failure(opened.error());
  }
  auto cursor = std::move(opened).value();
  const auto corrupt = [] {
    return Result<TempoEstimate>::failure({ErrorCode::corrupt_data, "Invalid tempo cache result"});
  };
  std::uint64_t bpm = 0;
  std::uint64_t confidence = 0;
  std::uint8_t truncated = 0;
  std::uint32_t count = 0;
  if (!cursor.read(bpm) || !cursor.read(confidence) || !cursor.read(truncated) ||
      !cursor.read(count) || truncated > 1 || count > max_tempo_suggestions ||
      cursor.remaining() < static_cast<std::size_t>(count) * sizeof(std::uint64_t) + 5) {
    return corrupt();
  }
  TempoEstimate result{.identity = expected,
                       .sample_rate = source.sample_rate,
                       .frames = source.frames,
                       .bpm = std::bit_cast<double>(bpm),
                       .confidence = std::bit_cast<double>(confidence),
                       .suggested_beat_frames = {},
                       .suggestions_truncated = truncated != 0,
                       .suggested_onset_frames = {},
                       .onsets_truncated = false};
  if (!std::isfinite(result.bpm) || !std::isfinite(result.confidence) || result.confidence < 0.0 ||
      result.confidence > 1.0 ||
      (result.bpm == 0.0 &&
       (result.confidence != 0.0 || count != 0 || result.suggestions_truncated)) ||
      (result.bpm != 0.0 &&
       (result.bpm < timeline::min_detected_bpm || result.bpm > timeline::max_detected_bpm))) {
    return corrupt();
  }
  result.suggested_beat_frames.reserve(count);
  std::uint64_t previous = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint64_t frame = 0;
    if (!cursor.read(frame) || frame >= source.frames || (index != 0 && frame <= previous)) {
      return corrupt();
    }
    result.suggested_beat_frames.push_back(frame);
    previous = frame;
  }
  std::uint8_t onsets_truncated = 0;
  std::uint32_t onset_count = 0;
  if (!cursor.read(onsets_truncated) || !cursor.read(onset_count) || onsets_truncated > 1 ||
      onset_count > max_tempo_suggestions || (onset_count == 0 && onsets_truncated != 0) ||
      cursor.remaining() != static_cast<std::size_t>(onset_count) * sizeof(std::uint64_t)) {
    return corrupt();
  }
  result.onsets_truncated = onsets_truncated != 0;
  result.suggested_onset_frames.reserve(onset_count);
  previous = 0;
  for (std::uint32_t index = 0; index < onset_count; ++index) {
    std::uint64_t frame = 0;
    if (!cursor.read(frame) || frame >= source.frames || (index != 0 && frame <= previous)) {
      return corrupt();
    }
    result.suggested_onset_frames.push_back(frame);
    previous = frame;
  }
  return Result<TempoEstimate>::success(std::move(result));
}

Result<std::vector<std::byte>> encode_key_cache(const KeyEstimate& estimate,
                                                std::uint32_t source_channels) {
  if (!valid_key_estimate(estimate, source_channels)) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::invalid_argument, "Invalid key cache result"});
  }
  constexpr std::size_t body_bytes = 1 + 1 + 1 + 8 + 1 + 3 * 3;
  const auto expected_bytes = 44 + estimate.identity.source_key.size() + body_bytes + 8;
  if (expected_bytes > max_payload_bytes) {
    return Result<std::vector<std::byte>>::failure(
        {ErrorCode::resource_exhausted, "Key cache exceeds payload budget"});
  }
  std::vector<std::byte> output;
  output.reserve(expected_bytes);
  append_header(output, key_kind, estimate.identity.source_key,
                {estimate.sample_rate, source_channels, estimate.frames},
                key_cache_payload_version);
  append(output, static_cast<std::uint8_t>(estimate.key.has_value()));
  append(output, estimate.key ? estimate.key->tonic : std::uint8_t{0});
  append(output, estimate.key ? static_cast<std::uint8_t>(estimate.key->mode) : std::uint8_t{0});
  append_double(output, estimate.confidence);
  append(output, static_cast<std::uint8_t>(estimate.compatible_keys.size()));
  for (const auto& compatible : estimate.compatible_keys) {
    append(output, compatible.key.tonic);
    append(output, static_cast<std::uint8_t>(compatible.key.mode));
    append(output, static_cast<std::uint8_t>(compatible.relation));
  }
  finish(output);
  return Result<std::vector<std::byte>>::success(std::move(output));
}

Result<KeyEstimate> decode_key_cache(std::span<const std::byte> bytes, const KeyIdentity& expected,
                                     SourceMetadata source) {
  auto opened = open(bytes, key_kind, expected, source, key_cache_payload_version);
  if (!opened) {
    return Result<KeyEstimate>::failure(opened.error());
  }
  auto cursor = std::move(opened).value();
  const auto corrupt = [] {
    return Result<KeyEstimate>::failure({ErrorCode::corrupt_data, "Invalid key cache result"});
  };
  std::uint8_t has_key = 0;
  std::uint8_t tonic = 0;
  std::uint8_t mode = 0;
  std::uint64_t confidence = 0;
  std::uint8_t count = 0;
  if (!cursor.read(has_key) || !cursor.read(tonic) || !cursor.read(mode) ||
      !cursor.read(confidence) || !cursor.read(count) || has_key > 1 || count > 3 ||
      cursor.remaining() != static_cast<std::size_t>(count) * 3) {
    return corrupt();
  }
  KeyEstimate result{
      .identity = expected,
      .sample_rate = source.sample_rate,
      .frames = source.frames,
      .key = has_key ? std::optional<MusicalKey>(MusicalKey{tonic, static_cast<KeyMode>(mode)})
                     : std::nullopt,
      .confidence = std::bit_cast<double>(confidence),
      .compatible_keys = {}};
  if (has_key == 0 && (tonic != 0 || mode != 0)) {
    return corrupt();
  }
  result.compatible_keys.reserve(count);
  for (std::uint8_t index = 0; index < count; ++index) {
    std::uint8_t related_tonic = 0;
    std::uint8_t related_mode = 0;
    std::uint8_t relation = 0;
    if (!cursor.read(related_tonic) || !cursor.read(related_mode) || !cursor.read(relation)) {
      return corrupt();
    }
    result.compatible_keys.push_back(
        {{related_tonic, static_cast<KeyMode>(related_mode)}, static_cast<KeyRelation>(relation)});
  }
  if (!valid_key_estimate(result, source.channels)) {
    return corrupt();
  }
  return Result<KeyEstimate>::success(std::move(result));
}

} // namespace oscillate::analysis
