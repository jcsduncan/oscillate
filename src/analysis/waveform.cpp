#include "oscillate/analysis/waveform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace oscillate::analysis {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;

constexpr std::size_t scratch_frames = 4'096;
constexpr std::size_t hard_bucket_limit = 1'000'000;
constexpr std::size_t hard_level_limit = 16;
constexpr std::size_t hard_identity_bytes = 128;

[[nodiscard]] std::uint64_t bucket_count(std::uint64_t frames, std::uint64_t width) {
  return frames / width + static_cast<std::uint64_t>(frames % width != 0);
}

[[nodiscard]] std::vector<std::uint64_t> level_widths(std::uint64_t frames,
                                                      const WaveformOptions& options) {
  std::vector<std::uint64_t> widths;
  widths.reserve(options.max_levels);
  auto width = std::min(options.minimum_frames_per_bucket, frames);
  for (std::size_t index = 0; index < options.max_levels; ++index) {
    widths.push_back(width);
    if (width >= frames) {
      break;
    }
    width = width > frames / 4 ? frames : width * 4;
  }
  return widths;
}

[[nodiscard]] bool fits_budget(std::uint64_t frames, const std::vector<std::uint64_t>& widths,
                               std::size_t maximum_buckets) {
  auto remaining = static_cast<std::uint64_t>(maximum_buckets);
  for (const auto width : widths) {
    const auto count = bucket_count(frames, width);
    if (count > remaining) {
      return false;
    }
    remaining -= count;
  }
  return true;
}

struct Accumulator {
  std::uint64_t count = 0;
  std::array<float, 2> minimum{};
  std::array<float, 2> maximum{};
  std::array<double, 2> sum_squares{};

  void add(const media::StereoFrame& frame, std::uint32_t channels) {
    const std::array values{frame.left, frame.right};
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      const auto value = values[channel];
      if (count == 0) {
        minimum[channel] = value;
        maximum[channel] = value;
      } else {
        minimum[channel] = std::min(minimum[channel], value);
        maximum[channel] = std::max(maximum[channel], value);
      }
      sum_squares[channel] += static_cast<double>(value) * static_cast<double>(value);
    }
    ++count;
  }

  [[nodiscard]] WaveformBucket finish(std::uint64_t begin, std::uint64_t end,
                                      std::uint32_t channels) const {
    WaveformBucket bucket{.begin_frame = begin, .end_frame = end};
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      bucket.channels[channel] = {.minimum = minimum[channel],
                                  .maximum = maximum[channel],
                                  .rms =
                                      std::sqrt(sum_squares[channel] / static_cast<double>(count))};
    }
    return bucket;
  }
};

[[nodiscard]] WaveformBucket combine(std::span<const WaveformBucket> children,
                                     std::uint32_t channels) {
  WaveformBucket combined{.begin_frame = children.front().begin_frame,
                          .end_frame = children.back().end_frame};
  const auto total_frames = combined.end_frame - combined.begin_frame;
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    auto minimum = children.front().channels[channel].minimum;
    auto maximum = children.front().channels[channel].maximum;
    double sum_squares = 0.0;
    for (const auto& child : children) {
      minimum = std::min(minimum, child.channels[channel].minimum);
      maximum = std::max(maximum, child.channels[channel].maximum);
      const auto rms = child.channels[channel].rms;
      sum_squares += rms * rms * static_cast<double>(child.end_frame - child.begin_frame);
    }
    combined.channels[channel] = {.minimum = minimum,
                                  .maximum = maximum,
                                  .rms =
                                      std::sqrt(sum_squares / static_cast<double>(total_frames))};
  }
  return combined;
}

} // namespace

Result<WaveformSummary> analyze_waveform(media::AudioReader& reader, WaveformIdentity identity,
                                         WaveformOptions options,
                                         const std::function<bool()>& cancelled) {
  if (identity.source_key.empty() || identity.source_key.size() > hard_identity_bytes ||
      identity.generation == 0 || options.minimum_frames_per_bucket == 0 ||
      options.max_total_buckets == 0 || options.max_total_buckets > hard_bucket_limit ||
      options.max_levels == 0 || options.max_levels > hard_level_limit) {
    return Result<WaveformSummary>::failure(
        {ErrorCode::invalid_argument, "Invalid waveform identity or analysis limits"});
  }
  const auto frames = reader.frame_count();
  const auto channels = reader.channels();
  if (reader.sample_rate() == 0 || (channels != 1 && channels != 2)) {
    return Result<WaveformSummary>::failure(
        {ErrorCode::unsupported, "Waveform reader requires mono or stereo audio"});
  }
  WaveformSummary result{.identity = std::move(identity),
                         .sample_rate = reader.sample_rate(),
                         .channels = channels,
                         .frames = frames,
                         .levels = {}};
  if (frames == 0) {
    return Result<WaveformSummary>::success(std::move(result));
  }

  auto widths = level_widths(frames, options);
  while (!fits_budget(frames, widths, options.max_total_buckets)) {
    const auto width = options.minimum_frames_per_bucket;
    options.minimum_frames_per_bucket = width > frames / 2 ? frames : width * 2;
    widths = level_widths(frames, options);
  }

  result.levels.reserve(widths.size());
  result.levels.push_back({.frames_per_bucket = widths.front(), .buckets = {}});
  auto& base = result.levels.front();
  base.buckets.reserve(static_cast<std::size_t>(bucket_count(frames, widths.front())));
  std::array<media::StereoFrame, scratch_frames> scratch{};
  Accumulator accumulator;
  std::uint64_t offset = 0;
  std::uint64_t bucket_begin = 0;
  const auto cancelled_error = [] {
    return Result<WaveformSummary>::failure({ErrorCode::cancelled, "Waveform analysis cancelled"});
  };
  while (offset < frames) {
    if (cancelled && cancelled()) {
      return cancelled_error();
    }
    const auto remaining = frames - offset;
    const auto wanted =
        static_cast<std::size_t>(std::min<std::uint64_t>(remaining, scratch.size()));
    auto read = reader.read_at(offset, std::span(scratch).first(wanted));
    if (!read) {
      return Result<WaveformSummary>::failure(read.error());
    }
    if (read.value() == 0 || read.value() > wanted) {
      return Result<WaveformSummary>::failure(
          {ErrorCode::corrupt_data, "Audio ended before verified waveform frame count"});
    }
    for (std::size_t index = 0; index < read.value(); ++index) {
      const auto& frame = scratch[index];
      if (!std::isfinite(frame.left) || (channels == 2 && !std::isfinite(frame.right))) {
        return Result<WaveformSummary>::failure(
            {ErrorCode::corrupt_data, "Non-finite audio sample in waveform source"});
      }
      accumulator.add(frame, channels);
      ++offset;
      if (accumulator.count == widths.front() || offset == frames) {
        base.buckets.push_back(accumulator.finish(bucket_begin, offset, channels));
        bucket_begin = offset;
        accumulator = {};
      }
    }
  }
  if (cancelled && cancelled()) {
    return cancelled_error();
  }

  for (std::size_t level = 1; level < widths.size(); ++level) {
    const auto& previous = result.levels.back().buckets;
    result.levels.push_back({.frames_per_bucket = widths[level], .buckets = {}});
    auto& current = result.levels.back().buckets;
    current.reserve(static_cast<std::size_t>(bucket_count(frames, widths[level])));
    for (std::size_t index = 0; index < previous.size();) {
      const auto begin = previous[index].begin_frame;
      const auto end = begin + std::min(widths[level], frames - begin);
      auto next = index;
      while (next < previous.size() && previous[next].end_frame <= end) {
        ++next;
      }
      current.push_back(combine(std::span(previous).subspan(index, next - index), channels));
      index = next;
    }
  }
  return Result<WaveformSummary>::success(std::move(result));
}

} // namespace oscillate::analysis
