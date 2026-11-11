#include "oscillate/analysis/tempo.hpp"
#include "oscillate/timeline/timing.hpp"

#include "dsp/offline_fft.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

// Constant-tempo analysis (ADR-0052). Released dance music, and nearly every
// produced track, holds one tempo, so the estimate is one straight grid:
//   1. Two onset curves, log spectral flux of the kick band (< 150 Hz) and of
//      the whole spectrum, each with its local mean removed.
//   2. A phase-free score per candidate tempo: the magnitude of each curve's
//      Fourier component at the beat rate and its 2x and 4x harmonics. Bands
//      are never summed before the magnitude, so offbeat hats (half a beat
//      from the kick) add to the score instead of cancelling it.
//   3. The most likely tempo, with a broad prior around dance tempi to settle
//      octave choices, then local pulse clarity in 8 s windows: tails, fades,
//      breakdowns and beatless intros get no weight in the final fit.
//   4. The weighted fit, refined to a thousandth of a BPM, snapped to a whole
//      BPM when the straight grid holds as well there.
//   5. The grid's phase from the kick (or, without one, the whole spectrum).
namespace oscillate::analysis {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
using Complex = std::complex<double>;

constexpr std::size_t scratch_frames = 4'096;
constexpr std::size_t hard_envelope_bins = 500'000;
constexpr std::size_t hard_suggestions = 50'000;
constexpr std::size_t hard_identity_bytes = 128;
constexpr double kick_band_hz = 150.0;
constexpr double detrend_seconds = 0.5;
constexpr double clarity_window_seconds = 8.0;
constexpr double clarity_hop_seconds = 2.0;
constexpr double prior_centre_bpm = 122.0;
constexpr double prior_octaves = 0.6;
// The coarse tempo search grid, then the finer one within reach of its best.
constexpr double coarse_bpm_step = 0.05;
constexpr double fine_bpm_step = 0.01;
constexpr double fine_bpm_reach = 1.5;
// Beat clarity is |component| / total onset strength in a window: about 1
// for a click track and 0.2-0.4 for produced music with a steady kick.
// Below the floor there is no beat; at `clear_clarity` it is certain.
constexpr double minimum_clarity = 0.1;
constexpr double clear_clarity = 0.25;
// A kick-band pulse at least this clear places the grid.
constexpr double kick_clarity_floor = 0.15;
constexpr std::array<std::pair<double, double>, 3> harmonics{{{1.0, 1.0}, {2.0, 0.5}, {4.0, 0.25}}};

[[nodiscard]] std::uint64_t ceil_div(std::uint64_t numerator, std::uint64_t denominator) {
  return numerator / denominator + static_cast<std::uint64_t>(numerator % denominator != 0);
}

[[nodiscard]] std::size_t next_power_of_two(std::size_t value) {
  std::size_t size = 1;
  while (size < value)
    size <<= 1U;
  return size;
}

[[nodiscard]] bool valid(const TempoIdentity& identity, const TempoOptions& options) {
  return !identity.source_key.empty() && identity.source_key.size() <= hard_identity_bytes &&
         identity.generation != 0 && std::isfinite(options.minimum_bpm) &&
         std::isfinite(options.maximum_bpm) && options.minimum_bpm >= timeline::min_detected_bpm &&
         options.maximum_bpm <= timeline::max_detected_bpm &&
         options.minimum_bpm < options.maximum_bpm && options.max_envelope_bins >= 2 &&
         options.max_envelope_bins <= hard_envelope_bins && options.max_suggestions > 0 &&
         options.max_suggestions <= hard_suggestions && options.max_onsets > 0 &&
         options.max_onsets <= hard_suggestions;
}

// The onset curves, one value per analysis frame. Frame i's window ends at
// source frame i * hop (the source is preceded by a window of silence, so a
// transient at its very start is seen arriving). Log flux rises as soon as
// a transient enters the window: frame i stands for source frame
// i * hop + offset.
struct Curves {
  std::vector<double> kick;
  std::vector<double> full;
  // Linear power rise in the kick band: unlike log flux, a kick's power
  // dwarfs the low end of a hat or a snare, so this marks where beats fall.
  std::vector<double> bass;
  std::uint64_t hop = 0;
  double rate = 0.0;        // frames per second
  double offset = 0.0;      // source frames, for the log flux curves
  double bass_offset = 0.0; // source frames, for the power curve
};
// How far before the window centre a transient peaks the curve, in windows
// (measured on clicks).
constexpr double onset_lead_windows = 0.35;
// Power rises later, as the transient nears the window's centre.
constexpr double bass_lead_windows = 0.10;

void remove_local_mean(std::vector<double>& curve, std::size_t width) {
  if (curve.empty() || width < 2)
    return;
  std::vector<double> prefix(curve.size() + 1, 0.0);
  for (std::size_t i = 0; i < curve.size(); ++i)
    prefix[i + 1] = prefix[i] + curve[i];
  const auto half = width / 2;
  std::vector<double> out(curve.size());
  for (std::size_t i = 0; i < curve.size(); ++i) {
    const auto from = i > half ? i - half : 0;
    const auto to = std::min(curve.size(), i + half + 1);
    const auto mean = (prefix[to] - prefix[from]) / static_cast<double>(to - from);
    out[i] = std::max(0.0, curve[i] - mean);
  }
  curve = std::move(out);
}

// The magnitude of a curve's (optionally weighted) Fourier component at
// `hz`, evaluated directly: exact at any frequency.
[[nodiscard]] Complex component(std::span<const double> curve, std::span<const double> weights,
                                double frame_rate, double hz) {
  const auto step = std::polar(1.0, -2.0 * std::numbers::pi * hz / frame_rate);
  Complex rotor{1.0, 0.0};
  Complex sum{0.0, 0.0};
  for (std::size_t i = 0; i < curve.size(); ++i) {
    const auto value = weights.empty() ? curve[i] : curve[i] * weights[i];
    sum += value * rotor;
    rotor *= step;
    if ((i & 1023U) == 1023U)
      rotor /= std::abs(rotor);
  }
  return sum;
}

// Each curve's zero-padded spectrum, for scoring many tempi at once.
struct Spectra {
  std::vector<std::vector<double>> magnitude;
  double bins_per_hz = 0.0;

  [[nodiscard]] double at(std::size_t curve, double hz) const {
    const auto& values = magnitude[curve];
    const auto position = hz * bins_per_hz;
    const auto below = static_cast<std::size_t>(position);
    if (below + 1 >= values.size() / 2)
      return 0.0;
    const auto fraction = position - static_cast<double>(below);
    return values[below] * (1.0 - fraction) + values[below + 1] * fraction;
  }
};

// Padded to four times the curve's length, the spectrum samples every peak
// at least four times across its width (which narrows as the source grows),
// so interpolation between bins finds it at any length.
[[nodiscard]] Spectra spectra(const std::array<const std::vector<double>*, 2>& curves,
                              std::span<const double> weights, double frame_rate) {
  const auto length = curves.front()->size();
  const auto size = next_power_of_two(4 * length);
  const native_dsp::dsp::OfflineFft transform(size);
  Spectra result;
  result.bins_per_hz = static_cast<double>(size) / frame_rate;
  std::vector<Complex> buffer(size);
  for (const auto* curve : curves) {
    std::ranges::fill(buffer, Complex{});
    for (std::size_t i = 0; i < length; ++i)
      buffer[i] = {weights.empty() ? (*curve)[i] : (*curve)[i] * weights[i], 0.0};
    transform.forward(buffer);
    auto& magnitude = result.magnitude.emplace_back(size);
    for (std::size_t i = 0; i < size; ++i)
      magnitude[i] = std::abs(buffer[i]);
  }
  return result;
}

[[nodiscard]] double score(const Spectra& spectra, double bpm) {
  double total = 0.0;
  for (std::size_t curve = 0; curve < spectra.magnitude.size(); ++curve)
    for (const auto& [multiple, gain] : harmonics)
      total += gain * spectra.at(curve, bpm * multiple / 60.0);
  return total;
}

[[nodiscard]] double exact_score(const std::array<const std::vector<double>*, 2>& curves,
                                 std::span<const double> weights, double frame_rate, double bpm) {
  double total = 0.0;
  for (const auto* curve : curves)
    for (const auto& [multiple, gain] : harmonics)
      total += gain * std::abs(component(*curve, weights, frame_rate, bpm * multiple / 60.0));
  return total;
}

[[nodiscard]] double prior(double bpm) {
  const auto octaves = std::log2(bpm / prior_centre_bpm) / prior_octaves;
  return std::exp(-0.5 * octaves * octaves);
}

struct Clarity {
  std::vector<double> weights;
  double confidence = 0.0;
};

// How clearly each stretch of the source pulses at `bpm`: per 8 s window, the
// best band's beat (or half-beat) component relative to its total onset
// strength. Windows well below the track's typical clarity get no weight.
[[nodiscard]] Clarity clarity(const std::array<const std::vector<double>*, 2>& curves,
                              double frame_rate, double bpm) {
  const auto length = curves.front()->size();
  const auto window =
      std::min<std::size_t>(length, static_cast<std::size_t>(clarity_window_seconds * frame_rate));
  const auto hop =
      std::max<std::size_t>(1, static_cast<std::size_t>(clarity_hop_seconds * frame_rate));
  std::vector<double> sum(length, 0.0);
  std::vector<double> count(length, 0.0);
  std::vector<double> beat_clarity;
  std::vector<double> all;
  for (std::size_t start = 0;; start += hop) {
    const auto from = std::min(start, length - window);
    double best = 0.0;
    double beat = 0.0;
    for (const auto* curve : curves) {
      const std::span segment(curve->data() + from, window);
      double total = 0.0;
      for (const auto value : segment)
        total += value;
      if (total <= 0.0)
        continue;
      for (const double multiple : {1.0, 2.0}) {
        // Relative to the window start: only the magnitude matters.
        const auto value =
            std::abs(component(segment, {}, frame_rate, bpm * multiple / 60.0)) / total;
        best = std::max(best, value);
        if (multiple == 1.0)
          beat = std::max(beat, value);
      }
    }
    all.push_back(best);
    beat_clarity.push_back(beat);
    for (std::size_t i = from; i < from + window; ++i) {
      sum[i] += best;
      count[i] += 1.0;
    }
    if (from + window >= length)
      break;
  }
  auto sorted = all;
  std::ranges::sort(sorted);
  const auto typical = sorted[std::min(sorted.size() - 1, sorted.size() * 3 / 4)];
  const auto threshold = std::max(0.5 * typical, 0.05);
  Clarity result;
  result.weights.resize(length);
  for (std::size_t i = 0; i < length; ++i) {
    const auto value = count[i] > 0.0 ? sum[i] / count[i] : 0.0;
    result.weights[i] = value >= threshold ? value : 0.0;
  }
  double clear = 0.0;
  std::size_t used = 0;
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i] >= threshold) {
      clear += beat_clarity[i];
      ++used;
    }
  }
  if (used > 0) {
    const auto mean = clear / static_cast<double>(used);
    const auto coverage = static_cast<double>(used) / static_cast<double>(all.size());
    result.confidence =
        mean < minimum_clarity
            ? 0.0
            : std::clamp((mean - minimum_clarity) / (clear_clarity - minimum_clarity), 0.0, 0.95) *
                  std::min(1.0, coverage / 0.3);
  }
  return result;
}

// Peaks of the whole-spectrum curve: the source's transients, for snapping.
[[nodiscard]] std::vector<std::uint64_t> onsets(const Curves& curves, std::uint64_t frames) {
  const auto& curve = curves.full;
  std::vector<std::uint64_t> result;
  if (curve.size() < 3)
    return result;
  const auto maximum = *std::ranges::max_element(curve);
  if (maximum <= 0.0)
    return result;
  const auto threshold = maximum * 0.10;
  const auto gap = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(curves.rate / 12.0));
  const auto at = [&](std::size_t i) {
    return std::min<std::uint64_t>(
        frames - 1, static_cast<std::uint64_t>(std::max(
                        0.0, std::round(static_cast<double>(i * curves.hop) + curves.offset))));
  };
  std::size_t last = std::numeric_limits<std::size_t>::max();
  for (std::size_t i = 0; i < curve.size(); ++i) {
    const auto value = curve[i];
    const auto before = i > 0 ? curve[i - 1] : 0.0;
    const auto after = i + 1 < curve.size() ? curve[i + 1] : 0.0;
    if (value < threshold || value < before || value <= after)
      continue;
    if (last != std::numeric_limits<std::size_t>::max() && i - last < gap) {
      if (value > curve[last]) {
        last = i;
        result.back() = at(i);
      }
      continue;
    }
    last = i;
    result.push_back(at(i));
  }
  return result;
}

} // namespace

Result<TempoEstimate> analyze_tempo(media::AudioReader& reader, TempoIdentity identity,
                                    TempoOptions options, const std::function<bool()>& cancelled) {
  if (!valid(identity, options)) {
    return Result<TempoEstimate>::failure(
        {ErrorCode::invalid_argument, "Invalid tempo identity or analysis limits"});
  }
  const auto sample_rate = reader.sample_rate();
  const auto channels = reader.channels();
  if (sample_rate == 0 || (channels != 1 && channels != 2)) {
    return Result<TempoEstimate>::failure(
        {ErrorCode::unsupported, "Tempo reader requires mono or stereo audio"});
  }
  // The analysis window grows with the rate; bound it as key analysis does.
  if (sample_rate > max_tempo_sample_rate) {
    return Result<TempoEstimate>::failure(
        {ErrorCode::unsupported, "Tempo analysis does not support this sample rate"});
  }
  const auto frames = reader.frame_count();
  TempoEstimate result{.identity = std::move(identity),
                       .sample_rate = sample_rate,
                       .frames = frames,
                       .bpm = 0.0,
                       .confidence = 0.0,
                       .suggested_beat_frames = {},
                       .suggestions_truncated = false,
                       .suggested_onset_frames = {},
                       .onsets_truncated = false};
  if (frames == 0) {
    return Result<TempoEstimate>::success(std::move(result));
  }
  const auto cancelled_error = [] {
    return Result<TempoEstimate>::failure({ErrorCode::cancelled, "Tempo analysis cancelled"});
  };

  // A 43 ms window at 48 kHz, scaled with the rate; eight hops per window,
  // or longer hops when the source would exceed the envelope budget.
  std::size_t window = 1024;
  while (window * 48'000 < static_cast<std::size_t>(sample_rate) * 2048)
    window <<= 1U;
  Curves curves;
  curves.hop = std::max<std::uint64_t>(
      window / 8, ceil_div(frames, static_cast<std::uint64_t>(options.max_envelope_bins)));
  curves.rate = static_cast<double>(sample_rate) / static_cast<double>(curves.hop);
  curves.offset = (onset_lead_windows - 0.5) * static_cast<double>(window);
  curves.bass_offset = (bass_lead_windows - 0.5) * static_cast<double>(window);
  const native_dsp::dsp::OfflineFft transform(window);
  std::vector<double> hann(window);
  for (std::size_t i = 0; i < window; ++i)
    hann[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                   static_cast<double>(window));
  const auto bins = window / 2 + 1;
  const auto kick_bins = std::max<std::size_t>(
      2, static_cast<std::size_t>(kick_band_hz * static_cast<double>(window) / sample_rate));
  std::vector<double> previous(bins, 0.0);
  std::vector<double> current(bins, 0.0);
  std::vector<double> previous_power(kick_bins, 0.0);
  std::vector<Complex> spectrum(window);
  // The current window of mono samples, ending at frame * hop.
  std::vector<double> samples(window, 0.0);
  std::array<media::StereoFrame, scratch_frames> scratch{};
  // A fine level envelope (a third of a millisecond per value) for placing
  // the grid on the attacks themselves.
  const std::size_t attack_bin = std::max<std::size_t>(16, sample_rate / 3'000);
  std::vector<float> attacks;
  attacks.reserve(static_cast<std::size_t>(frames / attack_bin + 1));
  double attack_energy = 0.0;
  std::size_t attack_count = 0;
  std::uint64_t read_offset = 0;
  std::size_t scratch_size = 0;
  std::size_t scratch_at = 0;
  const auto next_sample = [&](double& value) -> Result<bool> {
    if (scratch_at == scratch_size) {
      if (read_offset >= frames)
        return Result<bool>::success(false);
      if (cancelled && cancelled())
        return Result<bool>::failure({ErrorCode::cancelled, "Tempo analysis cancelled"});
      const auto wanted =
          static_cast<std::size_t>(std::min<std::uint64_t>(frames - read_offset, scratch.size()));
      auto read = reader.read_at(read_offset, std::span(scratch).first(wanted));
      if (!read)
        return Result<bool>::failure(read.error());
      if (read.value() == 0 || read.value() > wanted)
        return Result<bool>::failure(
            {ErrorCode::corrupt_data, "Audio ended before verified tempo frame count"});
      scratch_size = read.value();
      scratch_at = 0;
      read_offset += read.value();
    }
    const auto& frame = scratch[scratch_at++];
    if (!std::isfinite(frame.left) || (channels == 2 && !std::isfinite(frame.right)))
      return Result<bool>::failure(
          {ErrorCode::corrupt_data, "Non-finite audio sample in tempo source"});
    value = channels == 2 ? 0.5 * (static_cast<double>(frame.left) + frame.right)
                          : static_cast<double>(frame.left);
    attack_energy += value * value;
    if (++attack_count == attack_bin) {
      attacks.push_back(
          static_cast<float>(std::sqrt(attack_energy / static_cast<double>(attack_bin))));
      attack_energy = 0.0;
      attack_count = 0;
    }
    return Result<bool>::success(true);
  };
  // One frame of log flux from its magnitude spectrum.
  const auto add_frame = [&](const auto& magnitude) {
    double kick = 0.0;
    double full = 0.0;
    double bass = 0.0;
    for (std::size_t bin = 0; bin < bins; ++bin) {
      const auto value = magnitude(bin);
      current[bin] = std::log1p(100.0 * value);
      const auto rise = std::max(0.0, current[bin] - previous[bin]);
      full += rise;
      if (bin < kick_bins) {
        kick += rise;
        const auto power = value * value;
        bass += std::max(0.0, power - previous_power[bin]);
        previous_power[bin] = power;
      }
    }
    std::swap(current, previous);
    curves.kick.push_back(kick);
    curves.full.push_back(full);
    curves.bass.push_back(bass);
  };
  bool holding = false;
  bool exhausted = false;
  const auto analysis_frames = ceil_div(frames + window, curves.hop);
  curves.kick.reserve(static_cast<std::size_t>(analysis_frames));
  curves.full.reserve(static_cast<std::size_t>(analysis_frames));
  for (std::uint64_t frame = 0; frame < analysis_frames; ++frame) {
    // Slide the window on by a hop (the first window is all silence).
    const auto want = frame == 0 ? 0 : static_cast<std::size_t>(curves.hop);
    const auto shift = std::min(want, window);
    std::move(samples.begin() + static_cast<std::ptrdiff_t>(shift), samples.end(), samples.begin());
    for (std::size_t i = 0; i < want; ++i) {
      double value = 0.0;
      if (!exhausted) {
        auto got = next_sample(value);
        if (!got)
          return Result<TempoEstimate>::failure(got.error());
        exhausted = !got.value();
      }
      // Only the last `window` of a long hop stay in the window.
      if (i + window >= want)
        samples[window - want + i] = exhausted ? 0.0 : value;
    }
    // The input is real, so frames go two to a transform: one as the real
    // part and the next as the imaginary part, separated exactly after.
    const auto last_frame = frame + 1 == analysis_frames;
    if (!holding) {
      for (std::size_t i = 0; i < window; ++i)
        spectrum[i] = {samples[i] * hann[i], 0.0};
      holding = true;
      if (!last_frame)
        continue;
      transform.forward(spectrum);
      add_frame([&](std::size_t bin) { return std::abs(spectrum[bin]); });
    } else {
      for (std::size_t i = 0; i < window; ++i)
        spectrum[i].imag(samples[i] * hann[i]);
      transform.forward(spectrum);
      const auto mirror = [&](std::size_t bin) {
        return std::conj(spectrum[(window - bin) % window]);
      };
      add_frame([&](std::size_t bin) { return 0.5 * std::abs(spectrum[bin] + mirror(bin)); });
      add_frame([&](std::size_t bin) { return 0.5 * std::abs(spectrum[bin] - mirror(bin)); });
    }
    holding = false;
  }
  if (cancelled && cancelled())
    return cancelled_error();
  const auto detrend =
      std::max<std::size_t>(3, static_cast<std::size_t>(detrend_seconds * curves.rate));
  remove_local_mean(curves.kick, detrend);
  remove_local_mean(curves.full, detrend);
  remove_local_mean(curves.bass, detrend);

  const auto transients = onsets(curves, frames);
  result.suggested_onset_frames.assign(
      transients.begin(), transients.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                   transients.size(), options.max_onsets)));
  result.onsets_truncated = transients.size() > options.max_onsets;

  // Each curve scaled to unit total, so neither band dominates by loudness.
  for (auto* curve : {&curves.kick, &curves.full, &curves.bass}) {
    double total = 0.0;
    for (const auto value : *curve)
      total += value;
    if (total > 0.0)
      for (auto& value : *curve)
        value /= total;
  }
  const std::array<const std::vector<double>*, 2> bands{&curves.kick, &curves.full};
  // Too little material for a tempo: fewer than four beats' worth of pulses.
  if (transients.size() < 5 || curves.full.size() < 4)
    return Result<TempoEstimate>::success(std::move(result));

  // 2-3: the most likely tempo, then where the source actually pulses.
  const auto coarse = spectra(bands, {}, curves.rate);
  double best_bpm = 0.0;
  double best_value = 0.0;
  for (std::size_t step = 0;; ++step) {
    const auto bpm = options.minimum_bpm + static_cast<double>(step) * coarse_bpm_step;
    if (bpm > options.maximum_bpm)
      break;
    const auto value = score(coarse, bpm) * prior(bpm);
    if (value > best_value) {
      best_value = value;
      best_bpm = bpm;
    }
  }
  if (best_bpm <= 0.0)
    return Result<TempoEstimate>::success(std::move(result));
  if (cancelled && cancelled())
    return cancelled_error();
  const auto pulse = clarity(bands, curves.rate, best_bpm);
  if (pulse.confidence <= 0.0)
    return Result<TempoEstimate>::success(std::move(result));

  // 4: the weighted fit near that tempo, on the padded spectrum, then exactly.
  const auto weighted = spectra(bands, pulse.weights, curves.rate);
  double fit = best_bpm;
  double fit_value = 0.0;
  const auto fine_first = std::max(options.minimum_bpm, best_bpm - fine_bpm_reach);
  const auto fine_last = std::min(options.maximum_bpm, best_bpm + fine_bpm_reach);
  for (std::size_t step = 0;; ++step) {
    const auto bpm = fine_first + static_cast<double>(step) * fine_bpm_step;
    if (bpm > fine_last)
      break;
    const auto value = score(weighted, bpm);
    if (value > fit_value) {
      fit_value = value;
      fit = bpm;
    }
  }
  const auto exact = [&](double bpm) {
    return exact_score(bands, pulse.weights, curves.rate, bpm);
  };
  // Golden-section search within one spectrum bin either side: inside the
  // peak's main lobe, where the score has one maximum.
  constexpr double golden = 0.6180339887498949;
  const auto bin_bpm = 60.0 / weighted.bins_per_hz;
  double low = fit - bin_bpm;
  double high = fit + bin_bpm;
  double left = high - golden * (high - low);
  double right = low + golden * (high - low);
  double left_value = exact(left);
  double right_value = exact(right);
  while (high - low > 0.0005) {
    if (cancelled && cancelled())
      return cancelled_error();
    if (left_value < right_value) {
      low = left;
      left = right;
      left_value = right_value;
      right = low + golden * (high - low);
      right_value = exact(right);
    } else {
      high = right;
      right = left;
      right_value = left_value;
      left = high - golden * (high - low);
      left_value = exact(left);
    }
  }
  // The exact search may step a bin past the requested range near its ends.
  double bpm = std::clamp(0.5 * (low + high), options.minimum_bpm, options.maximum_bpm);
  const auto whole = std::round(bpm);
  if (std::abs(bpm - whole) < 0.05 && whole >= options.minimum_bpm &&
      whole <= options.maximum_bpm && exact(whole) >= 0.97 * exact(bpm))
    bpm = whole;

  // 5: the grid's phase. The kick's power marks the beat; without a kick,
  // the whole spectrum's beat component does.
  const auto hz = bpm / 60.0;
  const auto weighted_clarity = [&](const std::vector<double>& curve, Complex value) {
    double total = 0.0;
    for (std::size_t i = 0; i < curve.size(); ++i)
      total += curve[i] * pulse.weights[i];
    return total > 0.0 ? std::abs(value) / total : 0.0;
  };
  const auto bass = component(curves.bass, pulse.weights, curves.rate, hz);
  const auto full = component(curves.full, pulse.weights, curves.rate, hz);
  const auto period = 60.0 * sample_rate / bpm;
  // A component peaks where arg(z) = -2 pi hz t (t in seconds): its beat
  // position in source frames, within one period.
  const auto beat_at = [&](Complex value, double offset) {
    auto position = -std::arg(value) / (2.0 * std::numbers::pi * hz) * sample_rate + offset;
    position = std::fmod(position, period);
    return position < 0.0 ? position + period : position;
  };
  // The whole spectrum times the attack best, but offbeat hats can carry it
  // half a beat away. When there is a kick, its power decides which half.
  auto first = beat_at(full, curves.offset);
  if (weighted_clarity(curves.bass, bass) >= kick_clarity_floor) {
    const auto kick_at = beat_at(bass, curves.bass_offset);
    const auto distance = [&](double position) {
      const auto apart = std::abs(std::remainder(position - kick_at, period));
      return apart;
    };
    const auto other = std::fmod(first + period / 2.0, period);
    if (distance(other) < distance(first))
      first = other;
  }
  // 6: the attacks themselves. Around each beat of a pulsing stretch, the
  // steepest rise in level (relative to the loudest moment nearby) is where
  // that beat's transient starts; the grid moves by the median difference.
  // This measures attacks directly, whatever their shape.
  {
    const auto reach_bins =
        static_cast<std::ptrdiff_t>(0.04 * sample_rate / static_cast<double>(attack_bin));
    std::vector<double> differences;
    for (std::size_t beat = 0;; ++beat) {
      const auto position = first + static_cast<double>(beat) * period;
      if (position >= static_cast<double>(frames))
        break;
      const auto curve_index = static_cast<std::size_t>(
          std::max(0.0, std::round((position - curves.offset) / static_cast<double>(curves.hop))));
      if (curve_index >= pulse.weights.size() || pulse.weights[curve_index] <= 0.0)
        continue;
      const auto centre = static_cast<std::ptrdiff_t>(position / static_cast<double>(attack_bin));
      const auto from = std::max<std::ptrdiff_t>(0, centre - reach_bins);
      const auto to = std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(attacks.size()),
                                               centre + reach_bins + 1);
      if (to - from < 3)
        continue;
      float loudest = 0.0F;
      for (auto i = from; i < to; ++i)
        loudest = std::max(loudest, attacks[static_cast<std::size_t>(i)]);
      if (loudest <= 0.0F)
        continue;
      const auto floor = 0.05 * loudest;
      double steepest = 0.0;
      std::ptrdiff_t at = -1;
      for (auto i = from; i < to; ++i) {
        const auto before =
            i > 0 ? static_cast<double>(attacks[static_cast<std::size_t>(i - 1)]) : 0.0;
        const auto rise =
            std::log((attacks[static_cast<std::size_t>(i)] + floor) / (before + floor));
        if (rise > steepest) {
          steepest = rise;
          at = i;
        }
      }
      // A real attack at least doubles the level.
      if (at >= 0 && steepest >= std::log(2.0))
        differences.push_back(static_cast<double>(at * static_cast<std::ptrdiff_t>(attack_bin)) -
                              position);
    }
    if (differences.size() >= 4) {
      const auto middle = differences.begin() + static_cast<std::ptrdiff_t>(differences.size() / 2);
      std::ranges::nth_element(differences, middle);
      first = std::fmod(first + *middle, period);
      if (first < 0.0)
        first += period;
    }
  }
  // A beat within a millisecond of the start is the start.
  if (first < sample_rate / 1'000.0 || period - first < sample_rate / 1'000.0)
    first = 0.0;

  // Confidence also asks that the grid lands on transients: the share of
  // beats in pulsing stretches where the onset curve (whole spectrum or
  // kick) peaks within 35 ms at twice its level over the surrounding bar.
  const auto frame_at = [&](double position) {
    return (position - curves.offset) / static_cast<double>(curves.hop);
  };
  const auto reach = 0.035 * curves.rate;
  const auto bar = 2.0 * period / static_cast<double>(curves.hop);
  const auto length = curves.full.size();
  const auto peaks_at = [&](const std::vector<double>& curve, double centre) {
    const auto from = static_cast<std::size_t>(std::max(0.0, centre - reach));
    const auto to = std::min(length, static_cast<std::size_t>(std::max(0.0, centre + reach)) + 1);
    const auto around_from = static_cast<std::size_t>(std::max(0.0, centre - bar));
    const auto around_to =
        std::min(length, static_cast<std::size_t>(std::max(0.0, centre + bar)) + 1);
    if (from >= to || around_from >= around_to)
      return false;
    double peak = 0.0;
    for (auto i = from; i < to; ++i)
      peak = std::max(peak, curve[i]);
    double level = 0.0;
    for (auto i = around_from; i < around_to; ++i)
      level += curve[i];
    level /= static_cast<double>(around_to - around_from);
    return peak > 0.0 && peak >= 2.0 * level;
  };
  std::size_t considered = 0;
  std::size_t hit = 0;
  for (std::size_t beat = 0;; ++beat) {
    const auto position = first + static_cast<double>(beat) * period;
    if (position >= static_cast<double>(frames))
      break;
    const auto centre = frame_at(position);
    const auto index = static_cast<std::size_t>(std::max(0.0, std::round(centre)));
    if (index >= length || pulse.weights[index] <= 0.0)
      continue;
    ++considered;
    hit += static_cast<std::size_t>(peaks_at(curves.full, centre) || peaks_at(curves.bass, centre));
  }
  const auto landed =
      considered > 0 ? static_cast<double>(hit) / static_cast<double>(considered) : 0.0;
  // A tempo needs at least four beats that land on transients.
  if (hit < 4)
    return Result<TempoEstimate>::success(std::move(result));

  result.bpm = bpm;
  result.confidence = pulse.confidence * landed;
  for (std::size_t index = 0; index < options.max_suggestions; ++index) {
    const auto position = first + static_cast<double>(index) * period;
    if (position >= static_cast<double>(frames))
      break;
    // Rounding must not land on `frames`, one past the source.
    result.suggested_beat_frames.push_back(
        std::min(static_cast<std::uint64_t>(std::llround(position)), frames - 1));
  }
  result.suggestions_truncated =
      first + static_cast<double>(options.max_suggestions) * period < static_cast<double>(frames);
  if (cancelled && cancelled())
    return cancelled_error();
  return Result<TempoEstimate>::success(std::move(result));
}

bool conflicts_with_project_tempo(const TempoEstimate& estimate, double project_bpm) noexcept {
  return std::isfinite(project_bpm) && project_bpm > 0.0 && estimate.confidence >= 0.5 &&
         std::isfinite(estimate.bpm) && estimate.bpm > 0.0 &&
         std::abs(estimate.bpm - project_bpm) / project_bpm > 0.03;
}

} // namespace oscillate::analysis
