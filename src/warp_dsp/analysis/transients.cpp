#include "../core/source_window.hpp"
#include "oscillate/warp_dsp/analysis.hpp"
#include "oscillate/warp_dsp/real_fft.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <numeric>
#include <span>

namespace oscillate::warp_dsp::analysis {
namespace {

using foundation::ErrorCode;
using Result = foundation::Result<std::vector<Transient>>;

constexpr double bands_per_octave = 24.0;
constexpr double lowest_band_hz = 40.0;
constexpr double highest_band_hz = 16'000.0;
constexpr double percussive_split_hz = 2'000.0;
constexpr std::uint64_t max_seconds = 3'600;
// Analysis frames between cancellation polls.
constexpr std::size_t cancel_poll_frames = 256;
constexpr float absolute_flux_floor = 2.5F;
constexpr float relative_flux_offset = 0.08F;

// SuperFlux peak-picking windows (Böck & Widmer 2013).
constexpr double pre_max_seconds = 0.030;
constexpr double post_max_seconds = 0.030;
constexpr double pre_average_seconds = 0.100;
constexpr double post_average_seconds = 0.070;

struct Band {
  std::size_t first_bin;
  std::vector<float> weights;
  double centre_hz;
};

// Triangular, log-spaced filters; adjacent centres that land on the same FFT
// bin collapse so low bands never duplicate.
std::vector<Band> filterbank(std::size_t fft_size, std::uint32_t rate) {
  const double bin_hz = static_cast<double>(rate) / static_cast<double>(fft_size);
  const double top = std::min(highest_band_hz, 0.45 * rate);
  std::vector<std::size_t> centres;
  const auto band_limit =
      static_cast<int>(std::floor(bands_per_octave * std::log2(top / lowest_band_hz)));
  for (int step = 0; step <= band_limit; ++step) {
    const double frequency = lowest_band_hz * std::exp2(step / bands_per_octave);
    const auto bin = static_cast<std::size_t>(std::lround(frequency / bin_hz));
    if (centres.empty() || bin > centres.back())
      centres.push_back(bin);
  }
  std::vector<Band> bands;
  for (std::size_t index = 1; index + 1 < centres.size(); ++index) {
    const auto low = centres[index - 1];
    const auto centre = centres[index];
    const auto high = centres[index + 1];
    Band band{low, std::vector<float>(high - low + 1, 0.0F), static_cast<double>(centre) * bin_hz};
    for (auto bin = low; bin <= high; ++bin) {
      const double weight =
          bin <= centre ? static_cast<double>(bin - low) / static_cast<double>(centre - low)
                        : static_cast<double>(high - bin) / static_cast<double>(high - centre);
      band.weights[bin - low] = static_cast<float>(weight);
    }
    bands.push_back(std::move(band));
  }
  return bands;
}

constexpr std::size_t prediction_order = 16;
constexpr std::size_t prediction_context = 512;
constexpr double minimum_prediction_gain = 4.0;

// Linear-prediction coefficients (Levinson-Durbin on the autocorrelation of a
// Hann-windowed fit region). Returns zeros when the region is silent.
using Coefficients = std::array<double, prediction_order + 1>;

Coefficients prediction_coefficients(std::span<const float> fit) {
  const auto count = fit.size();
  std::vector<double> windowed(count);
  for (std::size_t index = 0; index < count; ++index)
    windowed[index] =
        fit[index] * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) /
                                           static_cast<double>(count)));
  Coefficients autocorrelation{};
  for (std::size_t lag = 0; lag <= prediction_order; ++lag) {
    for (std::size_t index = lag; index < count; ++index)
      autocorrelation[lag] += windowed[index] * windowed[index - lag];
  }
  Coefficients coefficients{};
  coefficients[0] = 1.0;
  if (autocorrelation[0] < 1e-12)
    return coefficients;
  // Slight lag windowing keeps the recursion well conditioned.
  autocorrelation[0] *= 1.0 + 1e-6;
  double error = autocorrelation[0];
  Coefficients previous{};
  for (std::size_t order = 1; order <= prediction_order; ++order) {
    double accumulator = autocorrelation[order];
    for (std::size_t index = 1; index < order; ++index)
      accumulator += coefficients[index] * autocorrelation[order - index];
    const double reflection = -accumulator / error;
    previous = coefficients;
    for (std::size_t index = 1; index < order; ++index)
      coefficients[index] = previous[index] + reflection * previous[order - index];
    coefficients[order] = reflection;
    error *= 1.0 - reflection * reflection;
    if (error <= 1e-12)
      break;
  }
  return coefficients;
}

// Attack start in `signal[context..]` by the AIC change-point picker (Maeda
// 1985) applied to the linear-prediction residual: a model fitted to the
// `context` samples before the search region predicts sustained material away,
// so a new note rising under a ringing one still stands out.
std::uint64_t refine_attack(std::span<const float> signal, std::size_t context,
                            std::int64_t first) {
  auto coefficients = prediction_coefficients(signal.first(context));
  const auto residual_at = [&](std::size_t index) {
    double residual = 0.0;
    for (std::size_t tap = 0; tap <= prediction_order; ++tap)
      residual += coefficients[tap] * signal[index - tap];
    return residual;
  };
  // Prediction only helps when the context is predictable (ringing tones);
  // over noise-like context (a cymbal or snare tail) it blurs low-frequency
  // attacks, so fall back to the raw signal below 6 dB of prediction gain.
  double signal_energy = 0.0;
  double residual_energy = 0.0;
  for (std::size_t index = prediction_order; index < context; ++index) {
    signal_energy += static_cast<double>(signal[index]) * signal[index];
    const double residual = residual_at(index);
    residual_energy += residual * residual;
  }
  if (residual_energy * minimum_prediction_gain >= signal_energy) {
    coefficients = {};
    coefficients[0] = 1.0;
  }
  const auto count = signal.size() - context;
  std::vector<double> sum(count + 1, 0.0);
  std::vector<double> square(count + 1, 0.0);
  for (std::size_t index = 0; index < count; ++index) {
    const double residual = residual_at(context + index);
    sum[index + 1] = sum[index] + residual;
    square[index + 1] = square[index] + residual * residual;
  }
  const auto variance = [&](std::size_t begin, std::size_t end) {
    const double n = static_cast<double>(end - begin);
    const double mean = (sum[end] - sum[begin]) / n;
    return std::max((square[end] - square[begin]) / n - mean * mean, 1e-20);
  };
  constexpr std::size_t margin = 4;
  std::size_t best = margin;
  double best_score = std::numeric_limits<double>::infinity();
  for (std::size_t split = margin; split + margin < count; ++split) {
    const double score = static_cast<double>(split) * std::log(variance(0, split)) +
                         static_cast<double>(count - split) * std::log(variance(split, count));
    if (score < best_score) {
      best_score = score;
      best = split;
    }
  }
  return static_cast<std::uint64_t>(
      std::max<std::int64_t>(first + static_cast<std::int64_t>(best), 0));
}

} // namespace

foundation::Result<std::vector<Transient>> detect_transients(SourceView& source,
                                                             const TransientOptions& options,
                                                             const std::atomic<bool>* cancelled) {
  const auto rate = options.sample_rate;
  if (rate < 8'000 || rate > 384'000 || !std::isfinite(options.threshold_scale) ||
      options.threshold_scale <= 0.0F || !std::isfinite(options.minimum_gap_seconds) ||
      options.minimum_gap_seconds < 0.0 || options.minimum_gap_seconds > 1.0)
    return Result::failure({ErrorCode::invalid_argument, "Invalid transient analysis options"});
  const auto frames = source.frames();
  if (frames > max_seconds * rate)
    return Result::failure(
        {ErrorCode::resource_exhausted, "Transient analysis is limited to one hour"});
  try {
    // Constant time resolution across rates: ~21 ms windows, ~2.7 ms hop.
    const std::size_t scale = rate > 66'150 ? 4 : rate > 33'075 ? 2 : 1;
    const std::size_t fft_size = 512 * scale;
    const std::size_t hop = 64 * scale;
    const std::size_t lag = fft_size / (2 * hop);
    // A window running past the source end sees an abrupt cut to padding,
    // which is not a musical onset; stop before any window leaves the source.
    if (frames < fft_size)
      return Result::success({});
    const auto analysis_frames = static_cast<std::size_t>((frames - fft_size / 2) / hop) + 1;
    RealFft fft(fft_size);
    AlignedBuffer input(fft_size);
    AlignedBuffer spectrum(fft_size);
    std::vector<float> window(fft_size);
    for (std::size_t index = 0; index < fft_size; ++index)
      window[index] = static_cast<float>(
          0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) /
                               static_cast<double>(fft_size)));
    const auto bands = filterbank(fft_size, rate);
    const auto band_count = bands.size();
    std::vector<float> magnitude(fft_size / 2 + 1);
    // Log-filtered spectra are kept for `lag` frames only.
    std::vector<std::vector<float>> history(lag + 1, std::vector<float>(band_count, 0.0F));
    std::vector<float> flux(analysis_frames, 0.0F);
    std::vector<float> high_flux(analysis_frames, 0.0F);
    detail::SourceWindow audio(fft_size * 64);
    for (std::size_t frame = 0; frame < analysis_frames; ++frame) {
      if (cancelled != nullptr && frame % cancel_poll_frames == 0 &&
          cancelled->load(std::memory_order_relaxed))
        return Result::failure({ErrorCode::cancelled, "Transient analysis cancelled"});
      // Frames are centred on frame * hop.
      const auto first =
          static_cast<std::int64_t>(frame * hop) - static_cast<std::int64_t>(fft_size / 2);
      if (!audio.ensure(source, first, fft_size))
        return Result::failure({ErrorCode::io_failure, "Transient analysis could not read source"});
      const float* left = audio.left(first);
      const float* right = audio.right(first);
      for (std::size_t index = 0; index < fft_size; ++index)
        input[index] = 0.5F * (left[index] + right[index]) * window[index];
      fft.forward(input.data(), spectrum.data());
      magnitude[0] = std::fabs(spectrum[0]);
      magnitude[fft_size / 2] = std::fabs(spectrum[1]);
      for (std::size_t bin = 1; bin < fft_size / 2; ++bin)
        magnitude[bin] = std::hypot(spectrum[2 * bin], spectrum[2 * bin + 1]);
      auto& current = history[frame % history.size()];
      for (std::size_t band = 0; band < band_count; ++band) {
        float energy = 0.0F;
        const auto& filter = bands[band];
        for (std::size_t tap = 0; tap < filter.weights.size(); ++tap)
          energy += filter.weights[tap] * magnitude[filter.first_bin + tap];
        current[band] = std::log10(1.0F + energy);
      }
      if (frame < lag)
        continue;
      // SuperFlux: rectified difference against a frequency max-filtered
      // earlier spectrum, which suppresses vibrato and tremolo.
      const auto& earlier = history[(frame - lag) % history.size()];
      float total = 0.0F;
      float high = 0.0F;
      for (std::size_t band = 0; band < band_count; ++band) {
        float reference = earlier[band];
        if (band > 0)
          reference = std::max(reference, earlier[band - 1]);
        if (band + 1 < band_count)
          reference = std::max(reference, earlier[band + 1]);
        const float rise = std::max(0.0F, current[band] - reference);
        total += rise;
        if (bands[band].centre_hz >= percussive_split_hz)
          high += rise;
      }
      flux[frame] = total;
      high_flux[frame] = high;
    }

    const double frames_per_second = static_cast<double>(rate) / static_cast<double>(hop);
    const auto span = [frames_per_second](double seconds) {
      return static_cast<std::size_t>(std::lround(seconds * frames_per_second));
    };
    const auto pre_max = span(pre_max_seconds);
    const auto post_max = span(post_max_seconds);
    const auto pre_average = span(pre_average_seconds);
    const auto post_average = span(post_average_seconds);
    const float peak_flux = *std::ranges::max_element(flux);
    std::vector<Transient> result;
    if (peak_flux <= 0.0F)
      return Result::success(std::move(result));
    // Adaptive threshold: local mean plus an offset relative to the
    // strongest onset, with an absolute floor so material without onsets
    // (vibrato, tremolo, fades) cannot promote its own modulation. Measured
    // log-filtered flux: sharp onsets 50-140, a vibrato pad at most 1.3.
    const float offset =
        std::max(absolute_flux_floor, relative_flux_offset * peak_flux) * options.threshold_scale;
    std::vector<double> cumulative(analysis_frames + 1, 0.0);
    for (std::size_t frame = 0; frame < analysis_frames; ++frame)
      cumulative[frame + 1] = cumulative[frame] + flux[frame];
    const auto gap_frames = static_cast<std::uint64_t>(
        std::llround(options.minimum_gap_seconds * static_cast<double>(rate)));
    std::vector<float> region(prediction_context + fft_size / 2 + hop * 2);
    std::vector<float> region_right(region.size());
    for (std::size_t frame = lag; frame < analysis_frames; ++frame) {
      if (cancelled != nullptr && frame % cancel_poll_frames == 0 &&
          cancelled->load(std::memory_order_relaxed))
        return Result::failure({ErrorCode::cancelled, "Transient analysis cancelled"});
      const float value = flux[frame];
      if (value <= 0.0F)
        continue;
      const auto max_begin = frame >= pre_max ? frame - pre_max : 0;
      const auto max_end = std::min(analysis_frames, frame + post_max + 1);
      if (*std::max_element(flux.begin() + static_cast<std::ptrdiff_t>(max_begin),
                            flux.begin() + static_cast<std::ptrdiff_t>(max_end)) > value)
        continue;
      const auto average_begin = frame >= pre_average ? frame - pre_average : 0;
      const auto average_end = std::min(analysis_frames, frame + post_average + 1);
      const double mean = (cumulative[average_end] - cumulative[average_begin]) /
                          static_cast<double>(average_end - average_begin);
      if (value < mean + offset)
        continue;
      // The rise against `lag` frames earlier puts the attack between the
      // earlier window's centre and this window's centre. The prediction
      // model is fitted to the context just before that span.
      const auto search_first = static_cast<std::int64_t>((frame - lag) * hop);
      const auto read_first = search_first - static_cast<std::int64_t>(prediction_context);
      const auto search_count = std::min<std::size_t>(
          fft_size / 2 + hop * 2, static_cast<std::size_t>(std::max<std::int64_t>(
                                      static_cast<std::int64_t>(frames) - search_first, 0)));
      if (search_count < 32)
        continue;
      const auto count = prediction_context + search_count;
      // Reads before the source start return silence.
      if (!source.read(read_first, std::span(region).first(count),
                       std::span(region_right).first(count)))
        return Result::failure({ErrorCode::io_failure, "Transient analysis could not read source"});
      for (std::size_t index = 0; index < count; ++index)
        region[index] = 0.5F * (region[index] + region_right[index]);
      const auto attack = refine_attack(std::span<const float>(region).first(count),
                                        prediction_context, search_first);
      if (attack >= frames)
        continue;
      if (!result.empty() && attack < result.back().frame + gap_frames) {
        // Keep the stronger of two onsets closer than the minimum gap.
        if (value / peak_flux > result.back().strength)
          result.back() = {attack, value / peak_flux, high_flux[frame] / value};
        continue;
      }
      if (result.size() == max_transients)
        break;
      result.push_back({attack, value / peak_flux, high_flux[frame] / value});
    }
    return Result::success(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result::failure({ErrorCode::resource_exhausted, "Transient analysis allocation failed"});
  }
}

} // namespace oscillate::warp_dsp::analysis
