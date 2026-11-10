#include "oscillate/analysis/mix_role.hpp"

#include "dsp/fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace oscillate::analysis {
namespace {

// Pitch is tracked at about this rate, after a box average, which keeps
// every pitch under pitch_high_hz.
constexpr double pitch_rate_hz = 16'000.0;
// The difference function's window, in decimated samples.
constexpr std::size_t pitch_window = 400;
// Onsets: a flux peak over this multiple of its neighbourhood's median.
constexpr double onset_ratio = 1.5;
constexpr std::size_t onset_neighbours = 8;
// Two onsets are at least this many frames apart.
constexpr std::size_t onset_gap_frames = 2;
constexpr double cents_per_octave = 1'200.0;
constexpr double cents_per_semitone = 100.0;
constexpr double a4_hz = 440.0;
constexpr double decibel_power_scale = 10.0;
constexpr double tiny = 1e-20;
constexpr std::size_t read_block_frames = 4'096;

struct Frame {
  double power = 0.0;
  double centroid_hz = 0.0;
  double low_power = 0.0;
  double flux = 0.0;
  double pitch_hz = 0.0; // 0 when unvoiced
};

// The frame's pitch by the cumulative mean normalised difference, with a
// parabolic step between lags; 0 when no lag dips under voiced_threshold.
[[nodiscard]] double frame_pitch(std::span<const float> decimated, double rate) {
  const auto shortest = static_cast<std::size_t>(std::floor(rate / pitch_high_hz));
  const auto longest = static_cast<std::size_t>(std::ceil(rate / pitch_low_hz));
  if (decimated.size() < pitch_window + longest + 1)
    return 0.0;
  std::vector<double> difference(longest + 2, 0.0);
  for (std::size_t lag = 1; lag <= longest + 1; ++lag) {
    double sum = 0.0;
    for (std::size_t i = 0; i < pitch_window; ++i) {
      const double delta = static_cast<double>(decimated[i]) - decimated[i + lag];
      sum += delta * delta;
    }
    difference[lag] = sum;
  }
  std::vector<double> normalised(longest + 2, 1.0);
  double running = 0.0;
  for (std::size_t lag = 1; lag <= longest + 1; ++lag) {
    running += difference[lag];
    normalised[lag] = running <= tiny ? 1.0 : difference[lag] * static_cast<double>(lag) / running;
  }
  for (std::size_t lag = std::max<std::size_t>(shortest, 2); lag <= longest; ++lag) {
    if (normalised[lag] >= voiced_threshold)
      continue;
    while (lag + 1 <= longest && normalised[lag + 1] < normalised[lag])
      ++lag;
    const double before = normalised[lag - 1];
    const double at = normalised[lag];
    const double after = normalised[lag + 1];
    const double bend = before - 2.0 * at + after;
    const double shift = bend > tiny ? 0.5 * (before - after) / bend : 0.0;
    return rate / (static_cast<double>(lag) + shift);
  }
  return 0.0;
}

[[nodiscard]] double median(std::vector<double> values) {
  if (values.empty())
    return 0.0;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::ranges::nth_element(values, middle);
  return *middle;
}

} // namespace

HeardFeatures hear_features(std::span<const float> mono, std::uint32_t sample_rate) {
  HeardFeatures features;
  if (sample_rate == 0 || mono.size() < hear_frame)
    return features;
  const double rate = static_cast<double>(sample_rate);
  const native_dsp::dsp::Fft transform(hear_frame);
  std::vector<double> window(hear_frame);
  for (std::size_t i = 0; i < hear_frame; ++i)
    window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                     static_cast<double>(hear_frame));
  const auto factor = std::max<std::size_t>(1, static_cast<std::size_t>(rate / pitch_rate_hz));
  const double pitch_rate = rate / static_cast<double>(factor);
  std::vector<float> decimated(mono.size() / factor);
  for (std::size_t i = 0; i < decimated.size(); ++i) {
    float sum = 0.0F;
    for (std::size_t k = 0; k < factor; ++k)
      sum += mono[i * factor + k];
    decimated[i] = sum / static_cast<float>(factor);
  }

  std::vector<Frame> frames;
  std::vector<std::complex<double>> data(hear_frame);
  std::vector<double> previous(hear_frame / 2 + 1, 0.0);
  const double bin_hz = rate / static_cast<double>(hear_frame);
  for (std::size_t start = 0; start + hear_frame <= mono.size(); start += hear_hop) {
    for (std::size_t i = 0; i < hear_frame; ++i)
      data[i] = {static_cast<double>(mono[start + i]) * window[i], 0.0};
    transform.forward(data);
    Frame frame;
    double weighted = 0.0;
    double magnitudes = 0.0;
    for (std::size_t bin = 1; bin <= hear_frame / 2; ++bin) {
      const double power = std::norm(data[bin]);
      const double magnitude = std::sqrt(power);
      const double hz = static_cast<double>(bin) * bin_hz;
      frame.power += power;
      if (hz < low_band_hz)
        frame.low_power += power;
      weighted += hz * magnitude;
      magnitudes += magnitude;
      const double level = std::log1p(magnitude);
      frame.flux += std::max(0.0, level - previous[bin]);
      previous[bin] = level;
    }
    frame.centroid_hz = magnitudes > tiny ? weighted / magnitudes : 0.0;
    const auto first = start / factor;
    if (first < decimated.size())
      frame.pitch_hz = frame_pitch(std::span(decimated).subspan(first), pitch_rate);
    frames.push_back(frame);
  }
  if (frames.empty())
    return features;

  const double loudest = std::ranges::max(frames, {}, &Frame::power).power;
  const double floor = loudest * std::pow(10.0, hear_silence_db / decibel_power_scale);
  std::vector<bool> sounding(frames.size());
  std::size_t count = 0;
  double centroid = 0.0;
  double low = 0.0;
  double total = 0.0;
  std::vector<double> pitches;
  double off_note = 0.0;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    sounding[i] = loudest > tiny && frames[i].power > floor;
    if (!sounding[i])
      continue;
    ++count;
    centroid += frames[i].centroid_hz;
    low += frames[i].low_power;
    total += frames[i].power;
    if (frames[i].pitch_hz > 0.0) {
      pitches.push_back(frames[i].pitch_hz);
      const double cents = cents_per_octave * std::log2(frames[i].pitch_hz / a4_hz);
      off_note += std::fabs(cents - cents_per_semitone * std::round(cents / cents_per_semitone));
    }
  }
  if (count == 0)
    return features;
  // Onsets: flux peaks over their neighbourhood's median, among sounding frames.
  std::size_t onsets = 0;
  std::size_t last = 0;
  bool any = false;
  for (std::size_t i = 1; i + 1 < frames.size(); ++i) {
    if (!sounding[i] || frames[i].flux <= frames[i - 1].flux || frames[i].flux < frames[i + 1].flux)
      continue;
    const auto from = i > onset_neighbours ? i - onset_neighbours : 0;
    const auto to = std::min(frames.size(), i + onset_neighbours + 1);
    std::vector<double> around;
    for (std::size_t k = from; k < to; ++k)
      around.push_back(frames[k].flux);
    if (frames[i].flux <= onset_ratio * median(around))
      continue;
    if (any && i - last < onset_gap_frames)
      continue;
    ++onsets;
    last = i;
    any = true;
  }
  // How far each sounding frame sits under its neighbourhood's loudest.
  const auto reach = static_cast<std::size_t>(decay_window_seconds * rate / hear_hop);
  std::vector<double> decays;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (!sounding[i])
      continue;
    const auto from = i > reach ? i - reach : 0;
    const auto to = std::min(frames.size(), i + reach + 1);
    double top = 0.0;
    for (std::size_t k = from; k < to; ++k)
      top = std::max(top, frames[k].power);
    decays.push_back(decibel_power_scale * std::log10(frames[i].power / top));
  }
  features.decay_db = median(decays);
  features.sounding_seconds = static_cast<double>(count) * hear_hop / rate;
  features.centroid_hz = centroid / static_cast<double>(count);
  features.low_share = total > tiny ? low / total : 0.0;
  features.onsets_per_second = static_cast<double>(onsets) / features.sounding_seconds;
  features.voiced_share = static_cast<double>(pitches.size()) / static_cast<double>(count);
  features.median_pitch_hz = median(pitches);
  features.off_note_cents = pitches.empty() ? 0.0 : off_note / static_cast<double>(pitches.size());
  return features;
}

project::MixRole role_from_features(const HeardFeatures& f) noexcept {
  if (f.sounding_seconds < hear_min_seconds)
    return project::MixRole::music;
  if (f.onsets_per_second >= drum_onsets_per_second && f.decay_db <= drum_max_decay_db)
    return project::MixRole::drums;
  if (f.voiced_share >= bass_min_voiced && f.median_pitch_hz < bass_max_pitch_hz &&
      f.low_share >= bass_min_low_share)
    return project::MixRole::bass;
  if (f.voiced_share >= vocal_min_voiced && f.median_pitch_hz >= vocal_min_pitch_hz &&
      f.median_pitch_hz <= vocal_max_pitch_hz && f.off_note_cents >= vocal_min_off_note_cents)
    return project::MixRole::vocals;
  return project::MixRole::music;
}

HeardRole hear_role(std::span<const float> mono, std::uint32_t sample_rate) {
  const auto features = hear_features(mono, sample_rate);
  return {role_from_features(features), features};
}

foundation::Result<HeardRole> hear_role(media::AudioReader& reader,
                                        const std::function<bool()>& cancelled) {
  using R = foundation::Result<HeardRole>;
  const auto rate = reader.sample_rate();
  const auto frames = reader.frame_count();
  const auto excerpt = static_cast<std::uint64_t>(hear_excerpt_seconds * rate);
  std::vector<std::uint64_t> starts;
  if (frames <= excerpt * hear_excerpts) {
    starts.push_back(0);
  } else {
    const auto stride = (frames - excerpt) / (hear_excerpts - 1);
    for (std::size_t i = 0; i < hear_excerpts; ++i)
      starts.push_back(stride * i);
  }
  const auto length = frames <= excerpt * hear_excerpts ? frames : excerpt;
  std::vector<float> mono;
  std::vector<media::StereoFrame> block(read_block_frames);
  for (const auto start : starts) {
    std::uint64_t at = start;
    const auto end = std::min(frames, start + length);
    while (at < end) {
      if (cancelled && cancelled())
        return R::failure({foundation::ErrorCode::cancelled, "Listening was cancelled"});
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), end - at));
      auto read = reader.read_at(at, std::span(block).first(want));
      if (!read)
        return R::failure(read.error());
      if (read.value() == 0)
        break;
      for (std::size_t i = 0; i < read.value(); ++i)
        mono.push_back(0.5F * (block[i].left + block[i].right));
      at += read.value();
    }
  }
  return R::success(hear_role(mono, rate));
}

} // namespace oscillate::analysis
