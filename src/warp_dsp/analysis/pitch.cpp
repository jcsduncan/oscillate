#include "../core/real_fft.hpp"
#include "../core/source_window.hpp"
#include "oscillate/warp_dsp/analysis.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <span>

namespace oscillate::warp_dsp::analysis {
namespace {

using foundation::ErrorCode;
constexpr std::uint64_t max_seconds = 3'600;
// The lowest pitch tracked or marked; its period always fits the mark arithmetic.
constexpr float lowest_pitch_hz = 20.0F;

[[nodiscard]] std::size_t power_of_two_at_least(std::size_t value) {
  std::size_t size = 32;
  while (size < value)
    size <<= 1;
  return size;
}

} // namespace

foundation::Result<std::vector<PitchFrame>>
track_pitch(SourceView& source, const PitchOptions& options, const std::atomic<bool>* cancelled) {
  using Result = foundation::Result<std::vector<PitchFrame>>;
  const auto rate = options.sample_rate;
  if (rate < 8'000 || rate > 384'000 || !std::isfinite(options.minimum_hz) ||
      !std::isfinite(options.maximum_hz) || options.minimum_hz < lowest_pitch_hz ||
      options.maximum_hz <= options.minimum_hz ||
      static_cast<double>(options.maximum_hz) > 0.25 * rate || !std::isfinite(options.threshold) ||
      options.threshold <= 0.0F || options.threshold >= 1.0F)
    return Result::failure({ErrorCode::invalid_argument, "Invalid pitch tracking options"});
  const auto frames = source.frames();
  if (frames > max_seconds * rate)
    return Result::failure(
        {ErrorCode::resource_exhausted, "Pitch tracking is limited to one hour"});
  try {
    const auto longest_lag =
        static_cast<std::size_t>(std::ceil(rate / static_cast<double>(options.minimum_hz)));
    const auto shortest_lag = std::max<std::size_t>(
        2, static_cast<std::size_t>(std::floor(rate / static_cast<double>(options.maximum_hz))));
    const std::size_t width = power_of_two_at_least(longest_lag + 2);
    const std::size_t size = 2 * width;
    const std::size_t hop = width / 4;
    detail::RealFft fft(size);
    detail::AlignedBuffer head(size);
    detail::AlignedBuffer whole(size);
    std::vector<double> squares(size + 1, 0.0);
    std::vector<double> difference(longest_lag + 2, 0.0);
    std::vector<double> normalised(longest_lag + 2, 1.0);
    detail::SourceWindow audio(size * 32);
    std::vector<PitchFrame> track;
    track.reserve(static_cast<std::size_t>(frames / hop) + 1);
    for (std::uint64_t centre = 0; centre < frames; centre += hop) {
      if (cancelled != nullptr && track.size() % 256 == 0 &&
          cancelled->load(std::memory_order_relaxed))
        return Result::failure({ErrorCode::cancelled, "Pitch tracking cancelled"});
      const auto first = static_cast<std::int64_t>(centre) - static_cast<std::int64_t>(width / 2);
      if (!audio.ensure(source, first, size))
        return Result::failure({ErrorCode::io_failure, "Pitch tracking could not read source"});
      const float* left = audio.left(first);
      const float* right = audio.right(first);
      for (std::size_t index = 0; index < size; ++index) {
        const float sample = 0.5F * (left[index] + right[index]);
        whole[index] = sample;
        head[index] = index < width ? sample : 0.0F;
        squares[index + 1] = squares[index] + static_cast<double>(sample) * sample;
      }
      PitchFrame frame{centre, 0.0F, 0.0F};
      const double energy = squares[width];
      if (energy < 1e-9 * static_cast<double>(width)) {
        track.push_back(frame);
        continue;
      }
      // Cross term of the YIN difference via one correlation in frequency.
      fft.forward(head.data(), head.data());
      fft.forward(whole.data(), whole.data());
      head[0] *= whole[0];
      head[1] *= whole[1];
      for (std::size_t bin = 1; bin < size / 2; ++bin) {
        const float re = head[2 * bin];
        const float im = -head[2 * bin + 1];
        const float other_re = whole[2 * bin];
        const float other_im = whole[2 * bin + 1];
        head[2 * bin] = re * other_re - im * other_im;
        head[2 * bin + 1] = re * other_im + im * other_re;
      }
      fft.inverse(head.data(), head.data());
      const double scale = 1.0 / static_cast<double>(size);
      double running = 0.0;
      for (std::size_t lag = 1; lag <= longest_lag + 1; ++lag) {
        const double shifted = squares[lag + width] - squares[lag];
        difference[lag] = std::max(0.0, energy + shifted - 2.0 * head[lag] * scale);
        running += difference[lag];
        normalised[lag] =
            running > 0.0 ? difference[lag] * static_cast<double>(lag) / running : 1.0;
      }
      // First dip below the threshold, followed to its local minimum.
      std::size_t best = 0;
      for (std::size_t lag = shortest_lag; lag <= longest_lag; ++lag) {
        if (normalised[lag] < options.threshold) {
          while (lag + 1 <= longest_lag && normalised[lag + 1] < normalised[lag])
            ++lag;
          best = lag;
          break;
        }
      }
      if (best == 0) {
        double lowest = 1.0;
        for (std::size_t lag = shortest_lag; lag <= longest_lag; ++lag)
          lowest = std::min(lowest, normalised[lag]);
        frame.clarity = static_cast<float>(std::max(0.0, 1.0 - lowest));
        track.push_back(frame);
        continue;
      }
      // Parabolic refinement on the raw difference.
      double offset = 0.0;
      if (best > shortest_lag && best < longest_lag) {
        const double before = difference[best - 1];
        const double at = difference[best];
        const double after = difference[best + 1];
        const double curvature = before - 2.0 * at + after;
        if (curvature > 0.0)
          offset = std::clamp(0.5 * (before - after) / curvature, -0.5, 0.5);
      }
      frame.frequency = static_cast<float>(rate / (static_cast<double>(best) + offset));
      frame.clarity = static_cast<float>(1.0 - normalised[best]);
      track.push_back(frame);
    }
    return Result::success(std::move(track));
  } catch (const std::bad_alloc&) {
    return Result::failure({ErrorCode::resource_exhausted, "Pitch tracking allocation failed"});
  }
}

foundation::Result<std::vector<PitchMark>> place_pitch_marks(SourceView& source,
                                                             const std::vector<PitchFrame>& track,
                                                             std::uint32_t sample_rate) {
  using Result = foundation::Result<std::vector<PitchMark>>;
  if (sample_rate < 8'000 || sample_rate > 384'000)
    return Result::failure({ErrorCode::invalid_argument, "Invalid pitch mark sample rate"});
  // Unvoiced frames (zero or below) stay allowed; a voiced frequency must
  // give a period the integer mark arithmetic can represent.
  if (std::ranges::any_of(track, [](const PitchFrame& frame) {
        return !std::isfinite(frame.frequency) ||
               (frame.frequency > 0.0F && frame.frequency < lowest_pitch_hz);
      }))
    return Result::failure({ErrorCode::invalid_argument, "Invalid pitch mark frequency"});
  try {
    std::vector<PitchMark> marks;
    const auto frames = source.frames();
    std::vector<float> left;
    std::vector<float> right;
    const auto period_at = [&track, sample_rate](std::uint64_t position, std::size_t& cursor) {
      while (cursor + 1 < track.size() && track[cursor + 1].frame <= position)
        ++cursor;
      const auto frequency = track[cursor].frequency;
      return frequency > 0.0F ? static_cast<float>(sample_rate) / frequency : 0.0F;
    };
    // Largest sample in [low, high) of the mono signal; -1 when unreadable.
    const auto peak_in = [&](std::int64_t low, std::int64_t high) -> std::int64_t {
      low = std::max<std::int64_t>(low, 0);
      high = std::min<std::int64_t>(high, static_cast<std::int64_t>(frames));
      if (high <= low)
        return -1;
      const auto count = static_cast<std::size_t>(high - low);
      left.resize(count);
      right.resize(count);
      if (!source.read(low, left, right))
        return -2;
      std::size_t best = 0;
      for (std::size_t index = 1; index < count; ++index) {
        if (left[index] + right[index] > left[best] + right[best])
          best = index;
      }
      return low + static_cast<std::int64_t>(best);
    };
    std::vector<float> reference;
    // Best-correlating start in [nominal - reach, nominal + reach] for one
    // period following `mark`; -1 past the source, -2 on a read failure.
    struct PeriodSearch {
      std::int64_t mark;
      std::int64_t nominal;
      std::int64_t reach;
      std::int64_t length;
    };
    const auto match_period = [&](const PeriodSearch& search) -> std::int64_t {
      const auto [mark, nominal, reach, length] = search;
      const auto low = nominal - reach;
      const auto high = nominal + reach + length;
      if (mark < 0 || low < 0 || high > static_cast<std::int64_t>(frames) || length <= 0)
        return -1;
      const auto span = static_cast<std::size_t>(high - mark);
      left.resize(span);
      right.resize(span);
      if (!source.read(mark, left, right))
        return -2;
      reference.resize(static_cast<std::size_t>(length));
      for (std::size_t index = 0; index < reference.size(); ++index)
        reference[index] = left[index] + right[index];
      double best_score = -1e300;
      std::int64_t best = nominal;
      for (auto candidate = low; candidate <= nominal + reach; ++candidate) {
        const auto offset = static_cast<std::size_t>(candidate - mark);
        double score = 0.0;
        for (std::size_t index = 0; index < reference.size(); ++index)
          score += static_cast<double>(reference[index]) *
                   (left[offset + index] + right[offset + index]);
        if (score > best_score) {
          best_score = score;
          best = candidate;
        }
      }
      return best;
    };
    std::size_t cursor = 0;
    std::size_t index = 0;
    while (index < track.size()) {
      if (track[index].frequency <= 0.0F) {
        ++index;
        continue;
      }
      // One voiced run of frames.
      auto run_end = index;
      while (run_end + 1 < track.size() && track[run_end + 1].frequency > 0.0F)
        ++run_end;
      const auto hop = track.size() > 1 ? track[1].frame - track[0].frame : 1;
      const auto run_first =
          static_cast<std::int64_t>(track[index].frame) - static_cast<std::int64_t>(hop / 2);
      const auto run_last = static_cast<std::int64_t>(track[run_end].frame + hop / 2);
      cursor = index;
      auto period =
          period_at(static_cast<std::uint64_t>(std::max<std::int64_t>(run_first, 0)), cursor);
      // A run voiced from the source start begins before frame 0; search the
      // first period inside the source, or the whole run would get no marks.
      const auto search_first = std::max<std::int64_t>(run_first, 0);
      auto mark =
          peak_in(search_first, search_first + static_cast<std::int64_t>(std::ceil(period)));
      while (mark >= 0 && mark < run_last) {
        if (marks.size() == max_pitch_marks)
          return Result::failure({ErrorCode::resource_exhausted, "Too many pitch marks"});
        period = period_at(static_cast<std::uint64_t>(mark), cursor);
        if (period <= 0.0F)
          break;
        if (!marks.empty() && static_cast<std::int64_t>(marks.back().frame) >= mark)
          break;
        marks.push_back({static_cast<std::uint64_t>(mark), period});
        // The next mark is the shift around one period on that best matches
        // this period's waveform, so marks stay on the same point of the
        // cycle even when a period has several near-equal peaks.
        const auto nominal = mark + static_cast<std::int64_t>(std::lround(period));
        const auto reach = static_cast<std::int64_t>(std::lround(0.25F * period));
        mark = match_period({mark, nominal, reach, static_cast<std::int64_t>(std::lround(period))});
      }
      if (mark == -2)
        return Result::failure({ErrorCode::io_failure, "Pitch marking could not read source"});
      index = run_end + 1;
    }
    return Result::success(std::move(marks));
  } catch (const std::bad_alloc&) {
    return Result::failure({ErrorCode::resource_exhausted, "Pitch marking allocation failed"});
  }
}

} // namespace oscillate::warp_dsp::analysis
