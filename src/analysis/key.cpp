#include "oscillate/analysis/key.hpp"
#include "oscillate/foundation/midi.hpp"

#include "dsp/fft.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace oscillate::analysis {
namespace {
using foundation::ErrorCode;
using Complex = std::complex<double>;
constexpr std::size_t window_frames = 16'384;
constexpr std::size_t hop_frames = 8'192;
constexpr std::size_t identity_limit = 128;
constexpr double silence_floor = 1e-5;
// Pitch evidence spans 55 Hz to 2 kHz: bass and harmony, little percussion.
constexpr double lowest_hz = 55.0;
constexpr double highest_hz = 2'000.0;
// The best key's profile correlation, and its lead over the runner-up,
// below which the key is unknown; at `confident_margin` it is certain.
constexpr double minimum_correlation = 0.5;
constexpr double minimum_margin = 0.01;
constexpr double confident_margin = 0.25;

[[nodiscard]] std::uint8_t pitch_class(int note) {
  return static_cast<std::uint8_t>(foundation::pitch_class(note));
}

// One shared table set for every analysis window (worker threads only).
void fft(std::array<Complex, window_frames>& values) {
  static const native_dsp::dsp::Fft transform(window_frames);
  transform.forward(values);
}

// Key profiles from KeyFinder (Sha'ath, 2011), tuned on popular and dance
// music: the weight of each scale degree, tonic first. On 26 tagged dance
// tracks these matched the labelled key more often than Krumhansl's,
// Temperley's or the EDM-specific profiles.
constexpr std::array<double, foundation::semitones_per_octave> major_profile{
    6.6, 2.0, 3.5, 2.3, 4.6, 4.0, 2.5, 5.2, 2.4, 3.7, 2.3, 3.4};
constexpr std::array<double, foundation::semitones_per_octave> minor_profile{
    6.5, 2.7, 3.5, 5.4, 2.6, 3.5, 2.5, 5.2, 4.0, 2.7, 4.3, 3.2};

// Pearson correlation of the chroma with the key's rotated profile.
[[nodiscard]] double key_score(const std::array<double, foundation::semitones_per_octave>& chroma,
                               MusicalKey key) {
  const auto& profile = key.mode == KeyMode::major ? major_profile : minor_profile;
  double chroma_mean = 0.0;
  double profile_mean = 0.0;
  for (std::size_t i = 0; i < foundation::semitones_per_octave; ++i) {
    chroma_mean += chroma[i] / static_cast<double>(foundation::semitones_per_octave);
    profile_mean += profile[i] / static_cast<double>(foundation::semitones_per_octave);
  }
  double covariance = 0.0;
  double chroma_spread = 0.0;
  double profile_spread = 0.0;
  for (std::size_t pitch = 0; pitch < foundation::semitones_per_octave; ++pitch) {
    const auto degree = pitch_class(static_cast<int>(pitch) - static_cast<int>(key.tonic));
    const auto c = chroma[pitch] - chroma_mean;
    const auto p = profile[degree] - profile_mean;
    covariance += c * p;
    chroma_spread += c * c;
    profile_spread += p * p;
  }
  return chroma_spread > 0.0 ? covariance / std::sqrt(chroma_spread * profile_spread) : 0.0;
}

// A key needs at least five pitch classes in play: one chord, however
// clear, does not name a key. Noise and ambiguity are left to the profile
// correlation and its margin.
[[nodiscard]] bool
enough_tonal_evidence(const std::array<double, foundation::semitones_per_octave>& chroma) {
  std::size_t populated = 0;
  for (const auto value : chroma) {
    populated += static_cast<std::size_t>(value >= 0.04);
  }
  return populated >= 5;
}

} // namespace

foundation::Result<KeyEstimate> analyze_key(media::AudioReader& reader, KeyIdentity identity,
                                            KeyOptions options,
                                            const std::function<bool()>& cancelled) {
  using Result = foundation::Result<KeyEstimate>;
  if (identity.source_key.empty() || identity.source_key.size() > identity_limit ||
      identity.generation == 0 || options.max_windows < 4 || options.max_windows > 1'024) {
    return Result::failure({ErrorCode::invalid_argument, "Invalid key analysis identity or limit"});
  }
  const auto rate = reader.sample_rate();
  const auto channels = reader.channels();
  const auto frames = reader.frame_count();
  if (rate < 8'000 || rate > 384'000 || (channels != 1 && channels != 2)) {
    return Result::failure(
        {ErrorCode::unsupported, "Key analysis requires mono/stereo PCM at a supported rate"});
  }
  KeyEstimate result{.identity = std::move(identity),
                     .sample_rate = rate,
                     .frames = frames,
                     .key = std::nullopt,
                     .confidence = 0.0,
                     .compatible_keys = {}};
  if (frames < window_frames) {
    return Result::success(std::move(result));
  }
  const auto possible = 1 + (frames - window_frames) / hop_frames;
  const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(possible, options.max_windows));
  // Pass 1: each window's spectral peaks between 55 Hz and 2 kHz, and the
  // summed spectrum, which sets the source's tuning.
  const auto lower = std::max<std::size_t>(
      2, static_cast<std::size_t>(std::ceil(lowest_hz * window_frames / rate)));
  const auto upper = std::min<std::size_t>(
      window_frames / 2 - 2, static_cast<std::size_t>(highest_hz * window_frames / rate));
  struct Peak {
    std::uint32_t bin;
    float magnitude;
  };
  std::vector<std::vector<Peak>> windows;
  windows.reserve(count);
  std::vector<double> summed(upper + 2, 0.0);
  std::array<media::StereoFrame, window_frames> input{};
  std::array<Complex, window_frames> spectrum{};
  std::vector<double> magnitude(upper + 2, 0.0);
  const auto cancel_result = [] {
    return Result::failure({ErrorCode::cancelled, "Key analysis cancelled"});
  };
  for (std::size_t window = 0; window < count; ++window) {
    if (cancelled && cancelled()) {
      return cancel_result();
    }
    std::uint64_t start = 0;
    if (count > 1) {
      const auto denominator = static_cast<std::uint64_t>(count - 1);
      const auto maximum = possible - 1;
      const auto quotient = maximum / denominator;
      const auto remainder = maximum % denominator;
      const auto index = static_cast<std::uint64_t>(window);
      start = (index * quotient + index * remainder / denominator) * hop_frames;
    }
    std::size_t read_total = 0;
    while (read_total < input.size()) {
      if (cancelled && cancelled()) {
        return cancel_result();
      }
      auto read = reader.read_at(start + read_total, std::span(input).subspan(read_total));
      if (!read) {
        return Result::failure(read.error());
      }
      if (read.value() == 0 || read.value() > input.size() - read_total) {
        return Result::failure({ErrorCode::corrupt_data, "Audio ended during key analysis"});
      }
      read_total += read.value();
    }
    double energy = 0.0;
    for (std::size_t index = 0; index < input.size(); ++index) {
      const auto& sample = input[index];
      if (!std::isfinite(sample.left) || (channels == 2 && !std::isfinite(sample.right))) {
        return Result::failure({ErrorCode::corrupt_data, "Non-finite key source sample"});
      }
      const auto value = channels == 2 ? 0.5 * (static_cast<double>(sample.left) + sample.right)
                                       : static_cast<double>(sample.left);
      energy += value * value;
      const auto hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(index) /
                                             static_cast<double>(window_frames - 1));
      spectrum[index] = {value * hann, 0.0};
    }
    auto& peaks = windows.emplace_back();
    if (energy / input.size() < silence_floor * silence_floor) {
      continue;
    }
    fft(spectrum);
    for (auto bin = lower - 1; bin <= upper + 1; ++bin) {
      magnitude[bin] = std::abs(spectrum[bin]);
    }
    for (auto bin = lower; bin <= upper; ++bin) {
      summed[bin] += magnitude[bin];
      if (magnitude[bin] > magnitude[bin - 1] && magnitude[bin] >= magnitude[bin + 1]) {
        peaks.push_back({static_cast<std::uint32_t>(bin), static_cast<float>(magnitude[bin])});
      }
    }
  }
  if (cancelled && cancelled()) {
    return cancel_result();
  }
  // The tuning: the energy-weighted circular mean of every bin's distance
  // from equal temperament at A = 440 Hz, in semitones.
  const auto semitone = [&](std::size_t bin) {
    return 69.0 + 12.0 * std::log2(static_cast<double>(bin) * rate / window_frames / 440.0);
  };
  Complex drift{};
  for (auto bin = lower; bin <= upper; ++bin) {
    const auto midi = semitone(bin);
    drift += summed[bin] * std::polar(1.0, 2.0 * std::numbers::pi * (midi - std::round(midi)));
  }
  const auto tuning = std::abs(drift) > 0.0 ? std::arg(drift) / (2.0 * std::numbers::pi) : 0.0;

  // Pass 2: each window's chroma, every peak weighted by the square root of
  // its magnitude and by how close it sits to a tuned semitone; each window
  // normalised, so every moment of the music counts once.
  std::vector<std::array<double, foundation::semitones_per_octave>> local(windows.size());
  std::vector<double> totals(windows.size(), 0.0);
  for (std::size_t window = 0; window < windows.size(); ++window) {
    for (const auto& peak : windows[window]) {
      const auto midi = semitone(peak.bin) - tuning;
      const auto nearest = std::round(midi);
      const auto weight = std::cos(std::numbers::pi * (midi - nearest));
      local[window][pitch_class(static_cast<int>(nearest))] +=
          std::sqrt(static_cast<double>(peak.magnitude)) * weight * weight;
    }
    for (const auto value : local[window]) {
      totals[window] += value;
    }
  }
  auto ordered = totals;
  std::ranges::sort(ordered);
  const auto typical = ordered.empty() ? 0.0 : ordered[ordered.size() / 2];
  std::array<double, foundation::semitones_per_octave> chroma{};
  std::size_t useful_windows = 0;
  for (std::size_t window = 0; window < windows.size(); ++window) {
    if (totals[window] <= 0.0 || totals[window] < 0.1 * typical) {
      continue;
    }
    for (std::size_t pitch = 0; pitch < chroma.size(); ++pitch) {
      chroma[pitch] += local[window][pitch] / totals[window];
    }
    ++useful_windows;
  }
  if (useful_windows < 8) {
    return Result::success(std::move(result));
  }
  for (auto& value : chroma) {
    value /= static_cast<double>(useful_windows);
  }
  if (!enough_tonal_evidence(chroma)) {
    return Result::success(std::move(result));
  }
  double best = -std::numeric_limits<double>::infinity();
  double runner = best;
  MusicalKey winner{};
  for (int tonic = 0; tonic < foundation::semitones_per_octave; ++tonic) {
    for (const auto mode : {KeyMode::major, KeyMode::minor}) {
      const MusicalKey candidate{static_cast<std::uint8_t>(tonic), mode};
      const auto score = key_score(chroma, candidate);
      if (score > best) {
        runner = best;
        best = score;
        winner = candidate;
      } else if (score > runner) {
        runner = score;
      }
    }
  }
  const auto margin = best - runner;
  if (best < minimum_correlation || margin < minimum_margin) {
    return Result::success(std::move(result));
  }
  result.key = winner;
  result.confidence = std::clamp(margin / confident_margin, 0.0, 0.85);
  const auto relative = winner.mode == KeyMode::major
                            ? MusicalKey{pitch_class(winner.tonic + 9), KeyMode::minor}
                            : MusicalKey{pitch_class(winner.tonic + 3), KeyMode::major};
  result.compatible_keys = {
      {relative, KeyRelation::relative},
      {{pitch_class(winner.tonic + 7), winner.mode}, KeyRelation::dominant},
      {{pitch_class(winner.tonic + 5), winner.mode}, KeyRelation::subdominant}};
  return Result::success(std::move(result));
}

} // namespace oscillate::analysis
