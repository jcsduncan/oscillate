#include "oscillate/analysis/mix_check.hpp"

#include "dsp/biquad.hpp"
#include "dsp/bs1770.hpp"
#include "dsp/fft.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <string_view>
#include <utility>

namespace oscillate::analysis::mix {
namespace {
using Complex = std::complex<double>;
namespace bs1770 = native_dsp::dsp::bs1770;

// The fine STFT resolves the lows (about 2.9 Hz bins at 48 kHz); the short
// one follows time (about 43 ms hops) for co-activity and decays. Both
// double at 88.2/96 kHz so their resolution in Hz is kept.
constexpr std::size_t fine_points = 16'384;
constexpr std::size_t short_points = 4'096;
constexpr std::uint32_t high_rate = 64'000;
// A hop is in a decay when it is no louder than the hop before and this far
// under the loudest of the last few hops.
constexpr double decay_below_db = 3.0;
constexpr std::size_t decay_lookback_hops = 4;
// Steps of 100 ms (BS.1770).
constexpr std::uint32_t steps_per_second = 10;
// EBU Tech 3342 loudness range: short-term values above the absolute gate,
// then 20 LU under their power mean, between these percentiles.
constexpr double range_relative_gate_lu = -20.0;
constexpr double range_low_percentile = 0.10;
constexpr double range_high_percentile = 0.95;
// Fourth-order Butterworth as two biquads.
constexpr std::array<double, 2> butterworth_q{0.5411961001461970, 1.3065629648763766};
constexpr double cents_per_octave = 1'200.0;
// A loss with nothing kept reads this much.
constexpr double total_loss_db = 60.0;
// The phrase-level loudness of silence.
constexpr double silent_lufs = -70.0;

[[nodiscard]] std::size_t scaled(std::size_t points, std::uint32_t rate) noexcept {
  return rate > high_rate ? points * 2 : points;
}

// Cutoff, Q, then rate: the RBJ cookbook's order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] native_dsp::dsp::BiquadCoefficients low_pass(double hz, double q,
                                                           double rate) noexcept {
  const double w = 2.0 * std::numbers::pi * hz / rate;
  const double alpha = std::sin(w) / (2.0 * q);
  const double cosine = std::cos(w);
  const double a0 = 1.0 + alpha;
  const double b0 = (1.0 - cosine) / 2.0 / a0;
  return {b0, 2.0 * b0, b0, -2.0 * cosine / a0, (1.0 - alpha) / a0};
}

[[nodiscard]] std::vector<double> hann(std::size_t size) {
  std::vector<double> window(size);
  for (std::size_t i = 0; i < size; ++i)
    window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                     static_cast<double>(size));
  return window;
}

// One Hann-windowed transform of a stereo frame, both channels packed into
// one complex FFT, as energy-normalised one-sided powers.
class Spectrogram {
public:
  explicit Spectrogram(std::size_t size)
      : size_(size), fft_(size), window_(hann(size)), data_(size), left_(size / 2 + 1),
        right_(size / 2 + 1), mid_(size / 2 + 1), side_(size / 2 + 1) {
    double energy = 0.0;
    for (const double w : window_)
      energy += w * w;
    scale_ = 1.0 / (static_cast<double>(size) * energy);
  }

  // frame_left/right hold `size` samples (zero-padded by the caller).
  void run(std::span<const double> frame_left, std::span<const double> frame_right) noexcept {
    for (std::size_t i = 0; i < size_; ++i)
      data_[i] = Complex(frame_left[i] * window_[i], frame_right[i] * window_[i]);
    fft_.forward(data_);
    const std::size_t half = size_ / 2;
    for (std::size_t k = 0; k <= half; ++k) {
      const Complex a = data_[k];
      const Complex b = std::conj(data_[(size_ - k) % size_]);
      const Complex l = (a + b) * 0.5;
      const Complex r = (a - b) * Complex(0.0, -0.5);
      const double one_sided = (k == 0 || k == half) ? 1.0 : 2.0;
      const double scale = scale_ * one_sided;
      left_[k] = std::norm(l) * scale;
      right_[k] = std::norm(r) * scale;
      mid_[k] = std::norm((l + r) * 0.5) * scale;
      side_[k] = std::norm((l - r) * 0.5) * scale;
    }
  }
  [[nodiscard]] const std::vector<double>& left() const noexcept { return left_; }
  [[nodiscard]] const std::vector<double>& right() const noexcept { return right_; }
  [[nodiscard]] const std::vector<double>& mid() const noexcept { return mid_; }
  [[nodiscard]] const std::vector<double>& side() const noexcept { return side_; }

private:
  std::size_t size_;
  native_dsp::dsp::Fft fft_;
  std::vector<double> window_;
  std::vector<Complex> data_;
  std::vector<double> left_;
  std::vector<double> right_;
  std::vector<double> mid_;
  std::vector<double> side_;
  double scale_ = 1.0;
};

// Each bin's band (band_count when it is outside every band).
[[nodiscard]] std::vector<std::size_t> bin_bands(std::size_t size, double bin_hz) {
  std::vector<std::size_t> bands(size / 2 + 1, band_count);
  for (std::size_t k = 0; k < bands.size(); ++k) {
    const double hz = static_cast<double>(k) * bin_hz;
    if (hz >= band_low(0) && hz < band_high(band_count - 1))
      bands[k] = band_of(hz);
  }
  return bands;
}

[[nodiscard]] double sum_bins(std::span<const double> bins, double bin_hz, double low_hz,
                              double high_hz) noexcept {
  double sum = 0.0;
  for (std::size_t k = 0; k < bins.size(); ++k) {
    const double hz = static_cast<double>(k) * bin_hz;
    if (hz >= low_hz && hz < high_hz)
      sum += bins[k];
  }
  return sum;
}

// Power per twelfth of an octave around `hz`: the bins' mean density times
// the window's width, so narrow low windows still read a level.
[[nodiscard]] double twelfth_octave(std::span<const double> bins, double bin_hz,
                                    double hz) noexcept {
  if (bins.empty() || bin_hz <= 0.0)
    return 0.0;
  const double half = std::pow(2.0, 1.0 / 24.0);
  const double low = hz / half;
  const double high = hz * half;
  const auto last = bins.size() - 1;
  auto first_bin = static_cast<std::size_t>(std::ceil(low / bin_hz));
  auto last_bin = static_cast<std::size_t>(std::floor(high / bin_hz));
  first_bin = std::min(first_bin, last);
  last_bin = std::min(last_bin, last);
  double sum = 0.0;
  std::size_t count = 0;
  for (auto k = first_bin; k <= last_bin && last_bin >= first_bin; ++k) {
    sum += bins[k];
    ++count;
  }
  if (count == 0) {
    sum = bins[std::min(last, static_cast<std::size_t>(std::lround(hz / bin_hz)))];
    count = 1;
  }
  return sum / static_cast<double>(count) * (high - low) / bin_hz;
}

[[nodiscard]] std::vector<double> stereo_bins(const Profile& profile) {
  std::vector<double> bins(profile.mid.size());
  for (std::size_t k = 0; k < bins.size(); ++k)
    bins[k] = profile.mid[k] + profile.side[k];
  return bins;
}

[[nodiscard]] BandPowers to_bands(std::span<const double> bins, double bin_hz) {
  BandPowers bands{};
  for (std::size_t k = 0; k < bins.size(); ++k) {
    const double hz = static_cast<double>(k) * bin_hz;
    if (hz >= band_low(0) && hz < band_high(band_count - 1))
      bands[band_of(hz)] += static_cast<float>(bins[k]);
  }
  return bands;
}

// Parabolic peak position (in bins) from three dB values.
[[nodiscard]] double refine(double before, double at, double after) noexcept {
  const double denominator = before - 2.0 * at + after;
  if (std::fabs(denominator) < 1e-12)
    return 0.0;
  return std::clamp(0.5 * (before - after) / denominator, -0.5, 0.5);
}

[[nodiscard]] bool contains_word(std::string_view text, std::string_view word) {
  std::string lower(text);
  std::ranges::transform(lower, lower.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find(word) != std::string::npos;
}

[[nodiscard]] double lufs_power(double lufs) noexcept {
  return std::pow(10.0, (lufs - bs1770::offset_lkfs) / 10.0);
}

} // namespace

// ---- Bands ---------------------------------------------------------------------------

double band_centre(std::size_t band) noexcept {
  return 1'000.0 *
         std::pow(2.0, (static_cast<double>(band) - static_cast<double>(reference_band)) / 3.0);
}
double band_low(std::size_t band) noexcept { return band_centre(band) * std::pow(2.0, -1.0 / 6.0); }
double band_high(std::size_t band) noexcept { return band_centre(band) * std::pow(2.0, 1.0 / 6.0); }
std::size_t band_of(double hz) noexcept {
  if (!(hz > 0.0))
    return 0;
  const double position = 3.0 * std::log2(hz / 1'000.0) + static_cast<double>(reference_band);
  const auto rounded = std::lround(position);
  return static_cast<std::size_t>(std::clamp<long>(rounded, 0, static_cast<long>(band_count - 1)));
}

double display_frequency(std::size_t point) noexcept {
  return display_low_hz *
         std::pow(display_high_hz / display_low_hz,
                  static_cast<double>(point) / static_cast<double>(display_points - 1));
}

double power_db(double power) noexcept {
  if (!(power > 0.0) || !std::isfinite(power))
    return silence_db;
  return std::max(silence_db, 10.0 * std::log10(power));
}

// ---- Profile -------------------------------------------------------------------------

double Profile::fine_bin_hz() const noexcept {
  return fine_size == 0 ? 0.0 : static_cast<double>(sample_rate) / static_cast<double>(fine_size);
}
double Profile::short_bin_hz() const noexcept {
  return short_size == 0 ? 0.0 : static_cast<double>(sample_rate) / static_cast<double>(short_size);
}
double Profile::seconds() const noexcept {
  return sample_rate == 0 ? 0.0 : static_cast<double>(frames) / static_cast<double>(sample_rate);
}
double Profile::hop_seconds() const noexcept {
  return sample_rate == 0 ? 0.0 : static_cast<double>(short_hop) / static_cast<double>(sample_rate);
}
double Profile::rms_db() const noexcept {
  if (frames == 0)
    return silence_db;
  return power_db((left_squares + right_squares) / (2.0 * static_cast<double>(frames)));
}
double Profile::peak_db() const noexcept { return power_db(sample_peak * sample_peak); }
double Profile::true_peak_db() const noexcept { return power_db(true_peak * true_peak); }

struct ProfileBuilder::State {
  State(std::uint32_t rate, ProfileOptions profile_options)
      : options(profile_options), fine(scaled(fine_points, rate)),
        brief(scaled(short_points, rate)) {
    result.sample_rate = rate;
    result.fine_size = scaled(fine_points, rate);
    result.short_size = scaled(short_points, rate);
    result.short_hop = result.short_size / 2;
    result.left.assign(result.fine_size / 2 + 1, 0.0);
    result.right.assign(result.fine_size / 2 + 1, 0.0);
    result.mid.assign(result.fine_size / 2 + 1, 0.0);
    result.side.assign(result.fine_size / 2 + 1, 0.0);
    result.sustain.assign(result.short_size / 2 + 1, 0.0);
    result.step_frames = std::max<std::uint32_t>(1, rate / steps_per_second);
    short_bands = bin_bands(result.short_size, result.short_bin_hz());
    frame_left.resize(result.fine_size);
    frame_right.resize(result.fine_size);
    hop_power.resize(result.short_size / 2 + 1);
    if (result.hop_seconds() > 0.0)
      decay_hops = std::max<std::size_t>(
          1, static_cast<std::size_t>(decay_follow_seconds / result.hop_seconds()));
    decay_sum.assign(decay_hops * hop_power.size(), 0.0);
    decay_count.assign(decay_hops, 0);
    hit_offset = decay_hops;
    if (options.detect_pitch)
      harmonic_hops.assign(hop_power.size(), 0);
    for (auto& weighting : k_weighting)
      weighting.prepare(static_cast<double>(rate));
  }

  void push(std::span<const float> left, std::span<const float> right) {
    const std::size_t count = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
      const double l = left[i];
      const double r = right[i];
      history_left.push_back(l);
      history_right.push_back(r);
      sample(l, r);
    }
    result.frames += count;
    analyse(false);
  }

  void sample(double l, double r) {
    result.sample_peak = std::max({result.sample_peak, std::fabs(l), std::fabs(r)});
    result.left_sum += l;
    result.right_sum += r;
    result.left_squares += l * l;
    result.right_squares += r * r;
    result.true_peak = std::max({result.true_peak, peaks[0].push(l), peaks[1].push(r)});
    const double kl = k_weighting[0].process(l);
    const double kr = k_weighting[1].process(r);
    step_sum += kl * kl + kr * kr;
    if (++step_filled == result.step_frames) {
      result.steps.push_back(step_sum / static_cast<double>(result.step_frames));
      step_sum = 0.0;
      step_filled = 0;
    }
  }

  // The frame at absolute `start`, zero-padded past the end.
  void load(std::uint64_t start, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
      const auto at = start + i;
      const bool inside = at >= history_start && at - history_start < history_left.size();
      frame_left[i] = inside ? history_left[static_cast<std::size_t>(at - history_start)] : 0.0;
      frame_right[i] = inside ? history_right[static_cast<std::size_t>(at - history_start)] : 0.0;
    }
  }

  // True when [start, start + size) holds no sound: its transform is zero,
  // so it is counted without being computed.
  [[nodiscard]] bool silent(std::uint64_t start, std::size_t size) const {
    const auto end = start + size;
    for (auto at = std::max(start, history_start);
         at < std::min<std::uint64_t>(end, history_start + history_left.size()); ++at) {
      const auto i = static_cast<std::size_t>(at - history_start);
      if (history_left[i] != 0.0 || history_right[i] != 0.0)
        return false;
    }
    return true;
  }

  void analyse(bool flush) {
    const auto end = history_start + history_left.size();
    const std::size_t fine_size = result.fine_size;
    const std::size_t short_size = result.short_size;
    while (flush ? fine_next < end : fine_next + fine_size <= end) {
      if (!silent(fine_next, fine_size)) {
        load(fine_next, fine_size);
        fine.run(std::span(frame_left).first(fine_size), std::span(frame_right).first(fine_size));
        for (std::size_t k = 0; k < result.left.size(); ++k) {
          result.left[k] += fine.left()[k];
          result.right[k] += fine.right()[k];
          result.mid[k] += fine.mid()[k];
          result.side[k] += fine.side()[k];
        }
      }
      ++fine_frames;
      fine_next += fine_size / 2;
    }
    while (flush ? short_next < end : short_next + short_size <= end) {
      short_hop(silent(short_next, short_size));
      short_next += result.short_hop;
    }
    // Drop history no frame still needs, in large steps.
    // Never past the input's end: a flush can step the hops beyond it.
    const auto keep_from = std::min<std::uint64_t>({fine_next, short_next, end});
    if (keep_from > history_start && keep_from - history_start >= fine_size) {
      const auto drop = static_cast<std::size_t>(keep_from - history_start);
      history_left.erase(history_left.begin(), history_left.begin() + static_cast<long>(drop));
      history_right.erase(history_right.begin(), history_right.begin() + static_cast<long>(drop));
      history_start = keep_from;
    }
  }

  // One short-STFT hop: its bands, whether it is in a decay, the hits'
  // decays and (audio) its pitch.
  void short_hop(bool quiet) {
    double total = 0.0;
    if (quiet) {
      std::ranges::fill(hop_power, 0.0);
    } else {
      load(short_next, result.short_size);
      brief.run(std::span(frame_left).first(result.short_size),
                std::span(frame_right).first(result.short_size));
      BandPowers bands{};
      for (std::size_t k = 0; k < short_bands.size(); ++k) {
        const double power = brief.mid()[k] + brief.side()[k];
        hop_power[k] = 0.5 * (brief.left()[k] + brief.right()[k]);
        total += power;
        if (short_bands[k] != band_count)
          bands[short_bands[k]] += static_cast<float>(power);
      }
      if (total > 0.0) {
        result.hop_index.push_back(static_cast<std::uint32_t>(result.hop_count));
        result.hop_bands.push_back(bands);
      }
    }
    ++result.hop_count;
    // In a decay: no louder than the hop before, and well under the
    // loudest of the last few.
    double recent = total;
    for (std::size_t back = 1; back <= decay_lookback_hops && back <= recent_totals.size(); ++back)
      recent = std::max(recent, recent_totals[recent_totals.size() - back]);
    const bool falling = !recent_totals.empty() && total <= recent_totals.back();
    const bool in_decay =
        total > 0.0 && falling && power_db(total) <= power_db(recent) - decay_below_db;
    if (in_decay) {
      for (std::size_t k = 0; k < result.sustain.size(); ++k)
        result.sustain[k] += hop_power[k];
      ++result.sustain_hops;
      if (options.detect_pitch)
        mark_harmonics(hop_pitch());
    }
    // A hit starts a new decay; each offset after it averages over the hits.
    const double before = recent_totals.empty() ? 0.0 : recent_totals.back();
    if (total > 0.0 && total >= before * std::pow(10.0, onset_rise_db / 10.0)) {
      hit_offset = 0;
      ++result.decay_hits;
    } else if (hit_offset < decay_hops) {
      ++hit_offset;
    }
    if (hit_offset < decay_hops) {
      auto row = std::span(decay_sum).subspan(hit_offset * hop_power.size(), hop_power.size());
      for (std::size_t k = 0; k < row.size(); ++k)
        row[k] += hop_power[k];
      ++decay_count[hit_offset];
    }
    recent_totals.push_back(total);
    if (recent_totals.size() > decay_lookback_hops)
      recent_totals.erase(recent_totals.begin());
  }

  // The hop's pitch (Hz) when its spectrum under pitch_top_hz is mostly one
  // harmonic series, else 0. The grid's best series is refined by a
  // power-weighted fit of its harmonics' interpolated peaks.
  [[nodiscard]] double hop_pitch() const {
    const double bin_hz = result.short_bin_hz();
    const auto top =
        std::min(hop_power.size() - 2, static_cast<std::size_t>(std::floor(pitch_top_hz / bin_hz)));
    double total = 0.0;
    for (auto k = static_cast<std::size_t>(std::ceil(pitch_low_hz / bin_hz)); k <= top; ++k)
      total += hop_power[k];
    if (!(total > 0.0))
      return 0.0;
    const auto nearest = [&](double hz) {
      return std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(hz / bin_hz)), 1, top);
    };
    const auto strongest_near = [&](double hz) {
      const auto k = nearest(hz);
      std::size_t best = k;
      for (const auto j : {k - 1, k + 1})
        if (hop_power[j] > hop_power[best])
          best = j;
      return best;
    };
    double best_sum = 0.0;
    double best_f0 = 0.0;
    for (double step = 0.0;; ++step) {
      const double f0 = pitch_low_hz * std::pow(2.0, step / pitch_steps_per_octave);
      if (f0 > pitch_high_hz)
        break;
      double sum = 0.0;
      for (std::size_t h = 1; h <= pitch_harmonics && static_cast<double>(h) * f0 < pitch_top_hz;
           ++h)
        sum += hop_power[strongest_near(static_cast<double>(h) * f0)];
      if (sum > best_sum) {
        best_sum = sum;
        best_f0 = f0;
      }
    }
    // A pitch is a series: several of its harmonics carry power.
    std::size_t partials = 0;
    for (std::size_t h = 1; h <= pitch_harmonics && static_cast<double>(h) * best_f0 < pitch_top_hz;
         ++h)
      if (hop_power[strongest_near(static_cast<double>(h) * best_f0)] >=
          pitch_partial_share * best_sum)
        ++partials;
    if (best_sum < pitched_share * total || partials < pitch_min_partials)
      return 0.0;
    double weighted_hz = 0.0;
    double weighted_harmonics = 0.0;
    for (std::size_t h = 1; h <= pitch_harmonics && static_cast<double>(h) * best_f0 < pitch_top_hz;
         ++h) {
      const auto k = strongest_near(static_cast<double>(h) * best_f0);
      const double weight = hop_power[k];
      if (!(weight > 0.0))
        continue;
      // A peak on the spectrum's edge has no neighbour to refine against.
      const double offset = k == 0 || k + 1 >= hop_power.size()
                                ? 0.0
                                : refine(power_db(hop_power[k - 1]), power_db(hop_power[k]),
                                         power_db(hop_power[k + 1]));
      weighted_hz += weight * (static_cast<double>(k) + offset) * bin_hz;
      weighted_harmonics += weight * static_cast<double>(h);
    }
    return weighted_harmonics > 0.0 ? weighted_hz / weighted_harmonics : best_f0;
  }

  void mark_harmonics(double pitch) {
    if (!(pitch > 0.0))
      return;
    const double bin_hz = result.short_bin_hz();
    const double spread = std::pow(2.0, note_harmonic_cents / cents_per_octave);
    for (std::size_t h = 1; h <= note_harmonics; ++h) {
      const double centre = pitch * static_cast<double>(h);
      const auto first = static_cast<std::size_t>(std::ceil(centre / spread / bin_hz));
      const auto last = static_cast<std::size_t>(std::floor(centre * spread / bin_hz));
      const auto nearest = static_cast<std::size_t>(std::lround(centre / bin_hz));
      for (auto k = std::min(first, nearest); k <= std::max(last, nearest); ++k)
        if (k < harmonic_hops.size())
          ++harmonic_hops[k];
    }
  }

  // Each bin's decay time and energy after the hits.
  void finish_decays() {
    std::size_t reached = 0;
    while (reached < decay_hops && decay_count[reached] >= decay_min_hits)
      ++reached;
    if (result.decay_hits < decay_min_hits || reached < 2)
      return;
    const auto bins = hop_power.size();
    const double hop = result.hop_seconds();
    result.decay.assign(bins, 0.0F);
    result.decay_energy.assign(bins, 0.0F);
    for (std::size_t k = 0; k < bins; ++k) {
      const auto mean = [&](std::size_t j) {
        return decay_sum[j * bins + k] / static_cast<double>(decay_count[j]);
      };
      std::size_t loudest = 0;
      double energy = 0.0;
      for (std::size_t j = 0; j < reached; ++j) {
        energy += mean(j);
        if (mean(j) > mean(loudest))
          loudest = j;
      }
      if (!(mean(loudest) > 0.0))
        continue;
      const double target = power_db(mean(loudest)) - decay_drop_db;
      double fallen = static_cast<double>(reached - 1);
      for (auto j = loudest + 1; j < reached; ++j) {
        const double now = power_db(mean(j));
        if (now > target)
          continue;
        const double was = power_db(mean(j - 1));
        fallen =
            static_cast<double>(j - 1) + (was - now > 0.0 ? (was - target) / (was - now) : 1.0);
        break;
      }
      result.decay[k] = static_cast<float>((fallen - static_cast<double>(loudest)) * hop);
      result.decay_energy[k] = static_cast<float>(energy);
    }
  }

  Profile finish() {
    for (std::size_t i = 0; i < bs1770::TruePeak::radius; ++i)
      result.true_peak = std::max({result.true_peak, peaks[0].push(0.0), peaks[1].push(0.0)});
    if (result.frames > 0)
      analyse(true);
    if (fine_frames > 0) {
      const auto frames = static_cast<double>(fine_frames);
      for (std::size_t k = 0; k < result.left.size(); ++k) {
        result.left[k] /= frames;
        result.right[k] /= frames;
        result.mid[k] /= frames;
        result.side[k] /= frames;
      }
    }
    if (result.sustain_hops > 0) {
      for (auto& bin : result.sustain)
        bin /= static_cast<double>(result.sustain_hops);
      if (options.detect_pitch) {
        result.harmonic_share.resize(harmonic_hops.size());
        for (std::size_t k = 0; k < harmonic_hops.size(); ++k)
          result.harmonic_share[k] = static_cast<float>(static_cast<double>(harmonic_hops[k]) /
                                                        static_cast<double>(result.sustain_hops));
      }
    }
    finish_decays();
    return std::move(result);
  }

  ProfileOptions options;
  Profile result;
  Spectrogram fine;
  Spectrogram brief;
  std::vector<std::size_t> short_bands;
  std::vector<double> history_left;
  std::vector<double> history_right;
  std::uint64_t history_start = 0;
  std::uint64_t fine_next = 0;
  std::uint64_t short_next = 0;
  std::uint64_t fine_frames = 0;
  std::vector<double> frame_left;
  std::vector<double> frame_right;
  std::vector<double> recent_totals;
  // The current hop's (L+R)/2 power per short bin.
  std::vector<double> hop_power;
  // The hits' decays: per offset after a hit, each bin's summed power and
  // how many hits reached it; hit_offset is decay_hops between hits.
  std::size_t decay_hops = 1;
  std::vector<double> decay_sum;
  std::vector<std::size_t> decay_count;
  std::size_t hit_offset = 0;
  std::vector<std::size_t> harmonic_hops;
  std::array<bs1770::KWeighting, 2> k_weighting{};
  std::array<bs1770::TruePeak, 2> peaks{};
  double step_sum = 0.0;
  std::uint32_t step_filled = 0;
};

ProfileBuilder::ProfileBuilder(std::uint32_t sample_rate, ProfileOptions options)
    : state_(std::make_unique<State>(sample_rate, options)) {}
ProfileBuilder::ProfileBuilder(ProfileBuilder&&) noexcept = default;
ProfileBuilder& ProfileBuilder::operator=(ProfileBuilder&&) noexcept = default;
ProfileBuilder::~ProfileBuilder() = default;

void ProfileBuilder::push(std::span<const float> left, std::span<const float> right) {
  state_->push(left, right);
}

Profile ProfileBuilder::finish() && { return state_->finish(); }

Profile profile(std::span<const float> left, std::span<const float> right,
                std::uint32_t sample_rate, ProfileOptions options) {
  ProfileBuilder builder(sample_rate, options);
  builder.push(left, right);
  return std::move(builder).finish();
}

BandPowers band_powers(const Profile& profile) {
  return to_bands(stereo_bins(profile), profile.fine_bin_hz());
}
BandPowers band_mid(const Profile& profile) { return to_bands(profile.mid, profile.fine_bin_hz()); }
BandPowers band_side(const Profile& profile) {
  return to_bands(profile.side, profile.fine_bin_hz());
}
BandPowers band_left(const Profile& profile) {
  return to_bands(profile.left, profile.fine_bin_hz());
}
BandPowers band_right(const Profile& profile) {
  return to_bands(profile.right, profile.fine_bin_hz());
}

double power_between(const Profile& profile, double low_hz, double high_hz) {
  return sum_bins(profile.mid, profile.fine_bin_hz(), low_hz, high_hz) +
         sum_bins(profile.side, profile.fine_bin_hz(), low_hz, high_hz);
}

std::vector<double> log_points(double low_hz, double high_hz, std::size_t points) {
  std::vector<double> x(points);
  for (std::size_t i = 0; i < points; ++i)
    x[i] = points < 2 ? low_hz
                      : low_hz * std::pow(high_hz / low_hz,
                                          static_cast<double>(i) / static_cast<double>(points - 1));
  return x;
}

std::vector<double> display_spectrum(const Profile& profile) {
  const auto bins = stereo_bins(profile);
  std::vector<double> y(display_points);
  for (std::size_t i = 0; i < display_points; ++i)
    y[i] = power_db(twelfth_octave(bins, profile.fine_bin_hz(), display_frequency(i)));
  return y;
}

std::vector<double> display_mono_spectrum(const Profile& profile) {
  std::vector<double> y(display_points);
  for (std::size_t i = 0; i < display_points; ++i)
    y[i] = power_db(twelfth_octave(profile.mid, profile.fine_bin_hz(), display_frequency(i)));
  return y;
}

std::vector<double> spectrum_points(const Profile& profile, double low_hz, double high_hz,
                                    std::size_t points, bool sustain) {
  const auto x = log_points(low_hz, high_hz, points);
  const auto bins = sustain ? profile.sustain : stereo_bins(profile);
  const double bin_hz = sustain ? profile.short_bin_hz() : profile.fine_bin_hz();
  std::vector<double> y(points);
  for (std::size_t i = 0; i < points; ++i)
    y[i] = power_db(twelfth_octave(bins, bin_hz, x[i]));
  return y;
}

// ---- Loudness ------------------------------------------------------------------------

Loudness loudness(const Profile& profile) {
  Loudness result;
  result.true_peak_db = profile.true_peak_db();
  const auto& steps = profile.steps;
  if (steps.size() >= bs1770::momentary_steps) {
    bs1770::Gate gate(steps.size());
    for (std::size_t i = 0; i + bs1770::momentary_steps <= steps.size(); ++i) {
      double sum = 0.0;
      for (std::size_t k = 0; k < bs1770::momentary_steps; ++k)
        sum += steps[i + k];
      (void)gate.add(sum / static_cast<double>(bs1770::momentary_steps));
    }
    result.integrated = gate.integrated().integrated_lkfs;
  }
  result.short_term.assign(steps.size(), std::nullopt);
  std::vector<double> gated;
  double running = 0.0;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    running += steps[i];
    if (i >= bs1770::short_term_steps)
      running -= steps[i - bs1770::short_term_steps];
    if (i + 1 < bs1770::short_term_steps)
      continue;
    const double power = std::max(0.0, running) / static_cast<double>(bs1770::short_term_steps);
    if (power <= 0.0)
      continue;
    const double value = bs1770::lkfs(power);
    result.short_term[i] = value;
    if (value > bs1770::absolute_gate_lkfs)
      gated.push_back(value);
  }
  if (gated.size() >= 2) {
    double sum = 0.0;
    for (const double value : gated)
      sum += lufs_power(value);
    const double gate =
        bs1770::lkfs(sum / static_cast<double>(gated.size())) + range_relative_gate_lu;
    std::vector<double> kept;
    for (const double value : gated)
      if (value > gate)
        kept.push_back(value);
    if (kept.size() >= 2) {
      std::ranges::sort(kept);
      const auto at = [&](double share) {
        const double position = share * static_cast<double>(kept.size() - 1);
        const auto below = static_cast<std::size_t>(std::floor(position));
        const auto above = std::min(kept.size() - 1, below + 1);
        const double t = position - static_cast<double>(below);
        return kept[below] + (kept[above] - kept[below]) * t;
      };
      result.range = at(range_high_percentile) - at(range_low_percentile);
    }
  }
  return result;
}

std::optional<double> steps_loudness(const Profile& profile, std::size_t first, std::size_t last) {
  last = std::min(last, profile.steps.size());
  if (first >= last)
    return std::nullopt;
  double sum = 0.0;
  for (auto i = first; i < last; ++i)
    sum += profile.steps[i];
  const double power = sum / static_cast<double>(last - first);
  if (!(power > 0.0))
    return std::nullopt;
  return bs1770::lkfs(power);
}

double crest_db(double peak, double mean_square) noexcept {
  if (!(peak > 0.0) || !(mean_square > 0.0))
    return 0.0;
  return 20.0 * std::log10(peak) - 10.0 * std::log10(mean_square);
}

// ---- Sub rumble ----------------------------------------------------------------------

std::optional<SubRumble> find_sub_rumble(const Profile& profile, double lowest_fundamental_hz) {
  if (!(lowest_fundamental_hz > 0.0) || profile.mid.empty())
    return std::nullopt;
  const double top = lowest_fundamental_hz * sub_edge_ratio;
  if (top <= sub_floor_hz)
    return std::nullopt;
  const double reference_low = lowest_fundamental_hz * std::pow(2.0, -1.0 / 24.0);
  const double reference_high = std::max(low_region_top_hz, lowest_fundamental_hz * 2.0);
  SubRumble rumble;
  rumble.sub_db = power_db(power_between(profile, sub_floor_hz, top));
  rumble.reference_db = power_db(power_between(profile, reference_low, reference_high));
  rumble.excess_db = rumble.sub_db - (rumble.reference_db - sub_allowance_db);
  if (rumble.sub_db <= silence_db || rumble.excess_db < sub_excess_threshold_db)
    return std::nullopt;
  // The band: the sub's strongest bin and its neighbours within
  // band_extent_db of it.
  const auto bins = stereo_bins(profile);
  const double bin_hz = profile.fine_bin_hz();
  const auto first = static_cast<std::size_t>(std::ceil(sub_floor_hz / bin_hz));
  const auto last = std::min(bins.size() - 1, static_cast<std::size_t>(std::floor(top / bin_hz)));
  std::size_t peak = first;
  for (auto k = first; k <= last; ++k)
    if (bins[k] > bins[peak])
      peak = k;
  const double floor = bins[peak] * std::pow(10.0, -band_extent_db / 10.0);
  auto low = peak;
  auto high = peak;
  while (low > first && bins[low - 1] >= floor)
    --low;
  while (high < last && bins[high + 1] >= floor)
    ++high;
  rumble.low_hz = std::max(sub_floor_hz, (static_cast<double>(low) - 0.5) * bin_hz);
  rumble.high_hz = std::min(top, (static_cast<double>(high) + 0.5) * bin_hz);
  return rumble;
}

std::optional<double> strongest_low_peak_hz(const Profile& profile) {
  const auto bins = stereo_bins(profile);
  const double bin_hz = profile.fine_bin_hz();
  if (bins.size() < 3 || bin_hz <= 0.0)
    return std::nullopt;
  const auto first =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(sub_floor_hz / bin_hz)));
  const auto last =
      std::min(bins.size() - 2, static_cast<std::size_t>(std::floor(low_region_top_hz / bin_hz)));
  if (first > last)
    return std::nullopt;
  std::size_t peak = first;
  for (auto k = first; k <= last; ++k)
    if (bins[k] > bins[peak])
      peak = k;
  if (power_db(bins[peak]) <= silence_db)
    return std::nullopt;
  const double offset =
      refine(power_db(bins[peak - 1]), power_db(bins[peak]), power_db(bins[peak + 1]));
  return (static_cast<double>(peak) + offset) * bin_hz;
}

double low_share(const Profile& profile) {
  const double total = power_between(profile, 0.0, static_cast<double>(profile.sample_rate));
  if (!(total > 0.0))
    return 0.0;
  return power_between(profile, 0.0, low_region_top_hz) / total;
}

std::optional<double> lowest_matching_peak_hz(const Profile& profile,
                                              std::span<const double> candidates_hz) {
  const auto bins = stereo_bins(profile);
  const double bin_hz = profile.fine_bin_hz();
  if (bins.size() < 3 || bin_hz <= 0.0 || candidates_hz.empty())
    return std::nullopt;
  const auto first =
      std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(sub_floor_hz / bin_hz)));
  const auto last =
      std::min(bins.size() - 2, static_cast<std::size_t>(std::floor(low_region_top_hz / bin_hz)));
  if (first > last)
    return std::nullopt;
  double strongest = 0.0;
  for (auto k = first; k <= last; ++k)
    strongest = std::max(strongest, bins[k]);
  if (power_db(strongest) <= silence_db)
    return std::nullopt;
  const double floor = strongest * std::pow(10.0, -fundamental_strong_db / 10.0);
  for (auto k = first; k <= last; ++k) {
    if (bins[k] < floor || bins[k] < bins[k - 1] || bins[k] < bins[k + 1])
      continue;
    const double hz = (static_cast<double>(k) +
                       refine(power_db(bins[k - 1]), power_db(bins[k]), power_db(bins[k + 1]))) *
                      bin_hz;
    const bool lines_up = std::ranges::any_of(candidates_hz, [&](double candidate) {
      if (!(candidate > 0.0))
        return false;
      const double tolerance =
          std::max(candidate * (std::pow(2.0, fundamental_match_cents / cents_per_octave) - 1.0),
                   fundamental_match_bins * bin_hz);
      return std::fabs(hz - candidate) <= tolerance;
    });
    if (lines_up)
      return hz;
  }
  return std::nullopt;
}

// ---- Mono safety ---------------------------------------------------------------------

std::array<double, band_count> band_mono_loss(const Profile& profile) {
  const auto mid = band_mid(profile);
  const auto side = band_side(profile);
  double loudest = 0.0;
  for (std::size_t b = 0; b < band_count; ++b)
    loudest = std::max(loudest, static_cast<double>(mid[b]) + side[b]);
  std::array<double, band_count> loss{};
  for (std::size_t b = 0; b < band_count; ++b) {
    const double total = static_cast<double>(mid[b]) + side[b];
    if (!(total > 0.0) || power_db(total) < power_db(loudest) - judged_range_db)
      continue;
    loss[b] = mid[b] > 0.0F ? -10.0 * std::log10(mid[b] / total) : total_loss_db;
  }
  return loss;
}

double span_mono_loss(const Profile& profile, std::size_t first_band, std::size_t last_band) {
  const auto mid = band_mid(profile);
  const auto side = band_side(profile);
  double kept = 0.0;
  double total = 0.0;
  for (auto b = first_band; b <= last_band && b < band_count; ++b) {
    kept += mid[b];
    total += static_cast<double>(mid[b]) + side[b];
  }
  if (!(total > 0.0))
    return 0.0;
  return kept > 0.0 ? -10.0 * std::log10(kept / total) : total_loss_db;
}

std::optional<MonoLoss> find_mono_loss(const Profile& profile) {
  const auto loss = band_mono_loss(profile);
  std::optional<MonoLoss> worst;
  for (std::size_t b = 0; b < band_count;) {
    if (loss[b] < mono_loss_threshold_db) {
      ++b;
      continue;
    }
    MonoLoss span{b, b, 0.0, 0.0};
    while (span.last_band + 1 < band_count && loss[span.last_band + 1] >= mono_loss_threshold_db)
      ++span.last_band;
    for (auto k = span.first_band; k <= span.last_band; ++k)
      span.worst_db = std::max(span.worst_db, loss[k]);
    span.loss_db = span_mono_loss(profile, span.first_band, span.last_band);
    if (!worst || span.loss_db > worst->loss_db)
      worst = span;
    b = span.last_band + 1;
  }
  return worst;
}

double low_side_db(const Profile& profile) {
  const double bin_hz = profile.fine_bin_hz();
  const double mid = sum_bins(profile.mid, bin_hz, sub_floor_hz, lock_crossover_hz);
  const double side = sum_bins(profile.side, bin_hz, sub_floor_hz, lock_crossover_hz);
  if (!(mid > 0.0))
    return side > 0.0 ? total_loss_db : silence_db;
  return power_db(side) - power_db(mid);
}

// ---- Resonances ----------------------------------------------------------------------

namespace {

[[nodiscard]] std::vector<double> sustain_db(const Profile& profile) {
  std::vector<double> db(profile.sustain.size());
  for (std::size_t k = 0; k < db.size(); ++k)
    db[k] = power_db(profile.sustain[k]);
  return db;
}

// The median dB of a bin's neighbourhood, its own peak bins left out.
[[nodiscard]] double smoothed_at(std::span<const double> db, std::size_t k,
                                 std::vector<double>& scratch) {
  const double spread = std::pow(2.0, resonance_neighbourhood_octaves);
  const auto low = static_cast<std::size_t>(std::floor(static_cast<double>(k) / spread));
  const auto high =
      std::min(db.size() - 1, static_cast<std::size_t>(std::ceil(static_cast<double>(k) * spread)));
  scratch.clear();
  for (auto j = low; j <= high; ++j) {
    const auto distance = j > k ? j - k : k - j;
    if (distance > resonance_peak_bins)
      scratch.push_back(db[j]);
  }
  if (scratch.empty())
    return db[k];
  const auto middle = scratch.begin() + static_cast<long>(scratch.size() / 2);
  std::nth_element(scratch.begin(), middle, scratch.end());
  return *middle;
}

[[nodiscard]] bool near_harmonic(double hz, std::span<const double> played_hz) {
  for (const double note : played_hz) {
    if (!(note > 0.0))
      continue;
    for (std::size_t h = 1; h <= note_harmonics; ++h) {
      const double harmonic = note * static_cast<double>(h);
      if (std::fabs(cents_per_octave * std::log2(hz / harmonic)) <= note_harmonic_cents)
        return true;
    }
  }
  return false;
}

} // namespace

namespace {

// The energy-weighted median decay of bins [low, high], bin k's own peak
// bins left out, and their energy.
struct Around {
  double seconds = 0.0;
  double energy = 0.0;
};
[[nodiscard]] std::optional<Around> weighted_decay(const Profile& profile, std::size_t k,
                                                   std::size_t low, std::size_t high) {
  std::vector<std::pair<float, float>> weighted;
  double total = 0.0;
  for (auto j = low; j <= high; ++j) {
    const auto distance = j > k ? j - k : k - j;
    if (distance <= resonance_peak_bins || !(profile.decay_energy[j] > 0.0F))
      continue;
    weighted.emplace_back(profile.decay[j], profile.decay_energy[j]);
    total += profile.decay_energy[j];
  }
  if (!(total > 0.0))
    return std::nullopt;
  std::ranges::sort(weighted);
  const double floor = decay_resolution_hops * profile.hop_seconds();
  double below = 0.0;
  for (const auto& [seconds, energy] : weighted) {
    below += energy;
    if (below >= total / 2.0)
      return Around{std::max(static_cast<double>(seconds), floor), total};
  }
  return Around{std::max(static_cast<double>(weighted.back().first), floor), total};
}

// How long the rest of the sound around bin k lasts: the frequencies within
// resonance_decay_octaves of it, or the whole sound when those carry less
// than resonance_context_db under k's own energy (a lone partial's
// neighbours hold only its own leakage).
[[nodiscard]] std::optional<double> decay_around(const Profile& profile, std::size_t k) {
  const auto last = profile.decay.size() - 1;
  double own = 0.0;
  for (auto j = k > resonance_peak_bins ? k - resonance_peak_bins : 0;
       j <= std::min(last, k + resonance_peak_bins); ++j)
    own += profile.decay_energy[j];
  const double spread = std::pow(2.0, resonance_decay_octaves);
  const auto low = static_cast<std::size_t>(std::floor(static_cast<double>(k) / spread));
  const auto high =
      std::min(last, static_cast<std::size_t>(std::ceil(static_cast<double>(k) * spread)));
  const auto near = weighted_decay(profile, k, low, high);
  if (near && near->energy >= own * std::pow(10.0, -resonance_context_db / 10.0))
    return near->seconds;
  const auto whole = weighted_decay(profile, k, 0, last);
  return whole ? std::optional<double>(whole->seconds) : std::nullopt;
}

} // namespace

std::vector<Resonance> find_resonances(const Profile& profile, std::span<const double> played_hz,
                                       std::size_t limit) {
  std::vector<Resonance> found;
  if (profile.sustain_hops < resonance_min_hops || profile.sustain.size() < 5 ||
      profile.decay.size() != profile.sustain.size())
    return found;
  const auto db = sustain_db(profile);
  const double bin_hz = profile.short_bin_hz();
  const double loudest = *std::ranges::max_element(db);
  const auto first =
      std::max<std::size_t>(2, static_cast<std::size_t>(std::ceil(resonance_low_hz / bin_hz)));
  const auto last =
      std::min(db.size() - 3, static_cast<std::size_t>(std::floor(resonance_high_hz / bin_hz)));
  std::vector<double> scratch;
  for (auto k = first; k <= last; ++k) {
    if (db[k] < db[k - 1] || db[k] < db[k + 1] || db[k] < db[k - 2] || db[k] < db[k + 2])
      continue;
    if (db[k] < loudest - resonance_audible_db)
      continue;
    const double prominence = db[k] - smoothed_at(db, k, scratch);
    if (prominence < resonance_threshold_db)
      continue;
    const double hz = (static_cast<double>(k) + refine(db[k - 1], db[k], db[k + 1])) * bin_hz;
    // The notes it plays, and the pitch an audio track sounds, are its own.
    if (near_harmonic(hz, played_hz) ||
        (!profile.harmonic_share.empty() && profile.harmonic_share[k] >= harmonic_share_threshold))
      continue;
    // It rings only when it outlasts the sound around it.
    const auto around = decay_around(profile, k);
    if (!around)
      continue;
    const double seconds = profile.decay[k];
    const double ratio = seconds / *around;
    if (ratio < resonance_decay_ratio)
      continue;
    const Resonance resonance{hz, prominence, db[k], seconds, ratio};
    // One resonance per merge distance: keep the longer-ringing.
    const auto close = std::ranges::find_if(found, [&](const Resonance& other) {
      return std::fabs(std::log2(other.hz / hz)) < resonance_merge_octaves;
    });
    if (close != found.end()) {
      if (close->decay_ratio < ratio)
        *close = resonance;
      continue;
    }
    found.push_back(resonance);
  }
  std::ranges::sort(found, [](const Resonance& a, const Resonance& b) {
    return a.decay_ratio != b.decay_ratio ? a.decay_ratio > b.decay_ratio
                                          : a.prominence_db > b.prominence_db;
  });
  if (found.size() > limit)
    found.resize(limit);
  return found;
}

// ---- Kick/bass cancellation ----------------------------------------------------------

LowBand::LowBand(std::uint32_t sample_rate) noexcept {
  for (std::size_t i = 0; i < coefficients_.size(); ++i) {
    const auto c = low_pass(lock_crossover_hz, butterworth_q[i], sample_rate);
    coefficients_[i] = {c.b0, c.b1, c.b2, c.a1, c.a2};
  }
}

void LowBand::process(std::span<const float> left, std::span<const float> right,
                      std::span<float> out) noexcept {
  const auto count = std::min({left.size(), right.size(), out.size()});
  for (std::size_t i = 0; i < count; ++i) {
    double x = 0.5 * (static_cast<double>(left[i]) + right[i]);
    // Transposed direct form II, one section after the other.
    for (std::size_t section = 0; section < coefficients_.size(); ++section) {
      const auto& c = coefficients_[section];
      auto& z = state_[section];
      const double y = c[0] * x + z[0];
      z[0] = c[1] * x - c[3] * y + z[1];
      z[1] = c[2] * x - c[4] * y;
      x = y;
    }
    out[i] = static_cast<float>(x);
  }
}

CancellationMeter::CancellationMeter(std::uint32_t sample_rate,
                                     std::vector<std::uint64_t> hit_frames)
    : sample_rate_(sample_rate), hits_(std::move(hit_frames)),
      window_frames_(std::max<std::uint64_t>(
          1, static_cast<std::uint64_t>(std::llround(dip_window_seconds * sample_rate)))),
      windows_(std::max<std::size_t>(
          1, static_cast<std::size_t>(std::llround(hit_window_seconds / dip_window_seconds)))),
      step_frames_(std::max<std::uint64_t>(
          1, static_cast<std::uint64_t>(std::llround(dip_plot_step_seconds * sample_rate)))),
      before_frames_(
          static_cast<std::uint64_t>(std::llround(dip_plot_before_seconds * sample_rate))),
      after_frames_(
          static_cast<std::uint64_t>(std::llround(dip_plot_after_seconds * sample_rate))) {
  std::ranges::sort(hits_);
}

void CancellationMeter::push(std::span<const float> kick_low, std::span<const float> other_low,
                             std::uint64_t first_frame) {
  const auto count = std::min(kick_low.size(), other_low.size());
  const auto span_frames = window_frames_ * windows_;
  for (std::size_t i = 0; i < count; ++i) {
    const auto frame = first_frame + i;
    const double k = kick_low[i];
    const double o = other_low[i];
    // The plot's steps: the low sum's energy, kept for the last moments.
    const auto step = frame / step_frames_;
    if (steps_.empty())
      steps_first_ = step;
    while (steps_first_ + steps_.size() <= step)
      steps_.push_back(0.0);
    steps_.back() += (k + o) * (k + o);
    // Hits that have started: open each, and close those that ended.
    while (next_hit_ + open_.size() < hits_.size() && hits_[next_hit_ + open_.size()] <= frame)
      open_.emplace_back(windows_);
    while (!open_.empty() && frame >= hits_[next_hit_] + span_frames) {
      close_hit(next_hit_);
      open_.erase(open_.begin());
      ++next_hit_;
    }
    for (std::size_t h = 0; h < open_.size(); ++h) {
      const auto window = static_cast<std::size_t>((frame - hits_[next_hit_ + h]) / window_frames_);
      auto& w = open_[h][window];
      w.kick += k * k;
      w.other += o * o;
      w.sum += (k + o) * (k + o);
    }
    end_frame_ = frame + 1;
    if (pending_envelope_ && end_frame_ >= hits_[*pending_envelope_] + after_frames_)
      take_envelope();
  }
  // Keep the steps the plot of an open or pending hit may still need.
  const auto keep_from_frame = [&] {
    std::uint64_t from = end_frame_;
    if (!open_.empty())
      from = std::min(from, hits_[next_hit_]);
    if (pending_envelope_)
      from = std::min(from, hits_[*pending_envelope_]);
    return from > before_frames_ ? from - before_frames_ : 0;
  }();
  const auto keep_step = keep_from_frame / step_frames_;
  if (keep_step > steps_first_) {
    const auto drop = std::min<std::uint64_t>(keep_step - steps_first_, steps_.size());
    steps_.erase(steps_.begin(), steps_.begin() + static_cast<std::ptrdiff_t>(drop));
    steps_first_ += drop;
  }
}

void CancellationMeter::close_hit(std::size_t hit) {
  const auto& windows = open_.front();
  double loudest = 0.0;
  for (const auto& w : windows)
    loudest = std::max(loudest, w.kick + w.other);
  const double range = std::pow(10.0, -overlap_range_db / 10.0);
  std::optional<double> dip;
  for (const auto& w : windows) {
    if (!(w.kick > 0.0) || !(w.other > 0.0) || w.kick + w.other < loudest * range ||
        w.other < w.kick * range || w.kick < w.other * range)
      continue;
    const double here = w.sum > 0.0 ? 10.0 * std::log10((w.kick + w.other) / w.sum) : total_loss_db;
    dip = std::max(dip.value_or(here), here);
  }
  if (!dip)
    return;
  dips_.push_back(*dip);
  if (!worst_ || *dip > worst_db_) {
    worst_ = hit;
    worst_db_ = *dip;
    pending_envelope_ = hit;
  }
}

void CancellationMeter::take_envelope() {
  const auto hit = hits_[*pending_envelope_];
  pending_envelope_.reset();
  envelope_.clear();
  const auto from = hit > before_frames_ ? hit - before_frames_ : 0;
  // Steps before the song's start read silence.
  const auto lead = static_cast<std::size_t>((before_frames_ - (hit - from)) / step_frames_);
  envelope_.assign(lead, silence_db);
  for (auto frame = from; frame < hit + after_frames_; frame += step_frames_) {
    const auto step = frame / step_frames_;
    const bool kept = step >= steps_first_ && step - steps_first_ < steps_.size();
    envelope_.push_back(kept ? power_db(steps_[static_cast<std::size_t>(step - steps_first_)] /
                                        static_cast<double>(step_frames_))
                             : silence_db);
  }
}

std::optional<Cancellation> CancellationMeter::finish() {
  while (!open_.empty()) {
    close_hit(next_hit_);
    open_.erase(open_.begin());
    ++next_hit_;
  }
  if (pending_envelope_)
    take_envelope();
  if (dips_.empty() || !worst_)
    return std::nullopt;
  auto sorted = dips_;
  std::ranges::sort(sorted);
  const double position = dip_percentile * static_cast<double>(sorted.size() - 1);
  const auto below = static_cast<std::size_t>(std::floor(position));
  const auto above = std::min(sorted.size() - 1, below + 1);
  const double fraction = position - static_cast<double>(below);
  Cancellation result;
  result.deepest_db = sorted[below] + (sorted[above] - sorted[below]) * fraction;
  result.worst_db = worst_db_;
  result.at_seconds = static_cast<double>(hits_[*worst_]) / static_cast<double>(sample_rate_);
  result.hits = dips_.size();
  result.envelope = envelope_;
  return result;
}

// ---- Drum crest ----------------------------------------------------------------------

DrumCrest::DrumCrest(std::size_t candidates)
    : candidates_(candidates), energy_((candidates + 1) * (candidates + 1), 0.0),
      sounded_(candidates, false) {}

void DrumCrest::push(std::span<const float> drums,
                     std::span<const std::span<const float>> candidates) {
  std::size_t count = drums.size();
  for (const auto lane : candidates)
    count = std::max(count, lane.size());
  if (count == 0)
    return;
  const auto at = [&](std::size_t lane, std::size_t i) -> double {
    const auto signal = lane == 0 ? drums : candidates[lane - 1];
    return i < signal.size() ? signal[i] : 0.0;
  };
  // The lanes that sound in this block: the known drums are lane 0.
  std::vector<std::size_t> active;
  for (std::size_t c = 0; c < candidates.size() && c < candidates_; ++c)
    if (std::ranges::any_of(candidates[c], [](float x) { return x != 0.0F; }))
      active.push_back(c + 1);
  const bool known = std::ranges::any_of(drums, [](float x) { return x != 0.0F; });
  any_known_ = any_known_ || known;
  for (const auto lane : active)
    sounded_[lane - 1] = true;
  // Energies and cross energies of the lanes that sound.
  std::vector<std::size_t> lanes;
  if (known)
    lanes.push_back(0);
  lanes.insert(lanes.end(), active.begin(), active.end());
  const auto stride = candidates_ + 1;
  for (std::size_t a = 0; a < lanes.size(); ++a)
    for (std::size_t b = a; b < lanes.size(); ++b) {
      double sum = 0.0;
      for (std::size_t i = 0; i < count; ++i)
        sum += at(lanes[a], i) * at(lanes[b], i);
      energy_[lanes[a] * stride + lanes[b]] += sum;
    }
  frames_ += count;
  float drums_peak = 0.0F;
  for (const float x : drums)
    drums_peak = std::max(drums_peak, std::fabs(x));
  if (active.empty()) {
    quiet_peak_ = std::max(quiet_peak_, drums_peak);
    return;
  }
  // The block's peak for each subset of the candidates that sound (the
  // known drums always in), or a bound when too many sound at once.
  Block block{lanes_.size(), peaks_.size(),
              static_cast<std::uint8_t>(
                  std::min<std::size_t>(active.size(), std::numeric_limits<std::uint8_t>::max())),
              active.size() <= crest_exact_lanes};
  for (const auto lane : active)
    lanes_.push_back(static_cast<std::uint32_t>(lane - 1));
  if (block.exact) {
    const std::size_t subsets = std::size_t{1} << active.size();
    const auto first = peaks_.size();
    peaks_.resize(first + subsets, 0.0F);
    for (std::size_t i = 0; i < count; ++i) {
      for (std::size_t mask = 0; mask < subsets; ++mask) {
        double sum = at(0, i);
        for (std::size_t bit = 0; bit < active.size(); ++bit)
          if ((mask >> bit) & 1U)
            sum += at(active[bit], i);
        peaks_[first + mask] = std::max(peaks_[first + mask], static_cast<float>(std::fabs(sum)));
      }
    }
  } else {
    peaks_.push_back(drums_peak);
    for (const auto lane : active) {
      float peak = 0.0F;
      for (const float x : candidates[lane - 1])
        peak = std::max(peak, std::fabs(x));
      peaks_.push_back(peak);
    }
  }
  blocks_.push_back(block);
}

std::optional<double> DrumCrest::crest_db(const std::vector<bool>& included) const {
  const auto in = [&](std::size_t candidate) {
    return candidate < included.size() && included[candidate];
  };
  bool any = any_known_;
  for (std::size_t c = 0; c < candidates_; ++c)
    any = any || (in(c) && sounded_[c]);
  if (!any || frames_ == 0)
    return std::nullopt;
  // Lane 0 and the included candidates: their energies and cross energies.
  const auto stride = candidates_ + 1;
  const auto lane_in = [&](std::size_t lane) { return lane == 0 || in(lane - 1); };
  double squares = 0.0;
  for (std::size_t a = 0; a < stride; ++a)
    for (std::size_t b = a; b < stride; ++b)
      if (lane_in(a) && lane_in(b))
        squares += (a == b ? 1.0 : 2.0) * energy_[a * stride + b];
  double peak = quiet_peak_;
  for (const auto& block : blocks_) {
    const auto lanes = std::span(lanes_).subspan(block.first_lane, block.lanes);
    if (block.exact) {
      std::size_t mask = 0;
      for (std::size_t bit = 0; bit < lanes.size(); ++bit)
        if (in(lanes[bit]))
          mask |= std::size_t{1} << bit;
      peak = std::max(peak, static_cast<double>(peaks_[block.first_peak + mask]));
    } else {
      double bound = peaks_[block.first_peak];
      for (std::size_t bit = 0; bit < lanes.size(); ++bit)
        if (in(lanes[bit]))
          bound += peaks_[block.first_peak + 1 + bit];
      peak = std::max(peak, bound);
    }
  }
  return analysis::mix::crest_db(peak, std::max(0.0, squares) / static_cast<double>(frames_));
}

// ---- Clicks --------------------------------------------------------------------------

std::optional<double> edge_click(std::span<const float> left, std::span<const float> right,
                                 std::size_t edge, std::uint32_t sample_rate) {
  const auto length = std::min(left.size(), right.size());
  if (edge >= length)
    return std::nullopt;
  // The sample before frame 0 is silence.
  double jump = 0.0;
  for (const auto channel : {left, right}) {
    const double before = edge == 0 ? 0.0 : static_cast<double>(channel[edge - 1]);
    jump = std::max(jump, std::fabs(static_cast<double>(channel[edge]) - before));
  }
  if (jump < click_floor)
    return std::nullopt;
  const auto context = std::max<std::size_t>(
      2, static_cast<std::size_t>(std::lround(click_context_seconds * sample_rate)));
  const auto first = edge > context ? edge - context : 1;
  const auto last = std::min(length - 1, edge + context);
  double squares = 0.0;
  std::size_t count = 0;
  for (const auto channel : {left, right}) {
    for (auto i = first; i <= last; ++i) {
      // The slope into sample i, skipping the edge's own and its neighbours.
      if (i + 1 >= edge && i <= edge + 1)
        continue;
      const double d = static_cast<double>(channel[i]) - static_cast<double>(channel[i - 1]);
      squares += d * d;
      ++count;
    }
  }
  const double local = count == 0 ? 0.0 : std::sqrt(squares / static_cast<double>(count));
  constexpr double tiny = 1e-9;
  if (jump < click_ratio * std::max(local, tiny))
    return std::nullopt;
  return 20.0 * std::log10(jump / std::max(local, tiny));
}

// ---- Sections ------------------------------------------------------------------------

double mean_lufs(std::span<const double> bar_lufs, std::size_t first, std::size_t last) {
  double sum = 0.0;
  std::size_t count = 0;
  for (auto b = first; b <= last && b < bar_lufs.size(); ++b) {
    if (bar_lufs[b] <= silent_lufs)
      continue;
    sum += lufs_power(bar_lufs[b]);
    ++count;
  }
  if (count == 0)
    return silence_db;
  return bs1770::lkfs(sum / static_cast<double>(count));
}

std::vector<Section> split_sections(std::span<const double> bar_lufs) {
  std::vector<Section> sections;
  for (std::size_t start = 0; start < bar_lufs.size(); start += phrase_bars) {
    const auto end = std::min(bar_lufs.size(), start + phrase_bars) - 1;
    const double phrase = mean_lufs(bar_lufs, start, end);
    if (!sections.empty()) {
      auto& current = sections.back();
      const bool both_silent = current.lufs <= silent_lufs && phrase <= silent_lufs;
      if (both_silent || std::fabs(phrase - current.lufs) < section_change_lu) {
        current.last_bar = end;
        current.lufs = mean_lufs(bar_lufs, current.first_bar, current.last_bar);
        continue;
      }
    }
    sections.push_back({{}, start, end, phrase});
  }
  rename_by_energy(sections);
  return sections;
}

void rename_by_energy(std::vector<Section>& sections) {
  if (sections.empty())
    return;
  double loudest = silence_db;
  for (const auto& section : sections)
    loudest = std::max(loudest, section.lufs);
  std::vector<bool> drop(sections.size());
  std::size_t first_drop = sections.size();
  std::size_t last_drop = 0;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    drop[i] = sections[i].lufs > silent_lufs && sections[i].lufs >= loudest - drop_window_lu;
    if (drop[i]) {
      first_drop = std::min(first_drop, i);
      last_drop = i;
    }
  }
  std::size_t drops = 0;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (drop[i])
      sections[i].name = "Drop " + std::to_string(++drops);
    else if (i < first_drop)
      sections[i].name = i == 0 ? "Intro" : "Build";
    else if (i > last_drop)
      sections[i].name = "Outro";
    else
      sections[i].name = "Break";
  }
}

std::optional<EnergyIssue> find_energy_issue(std::span<const Section> sections) {
  std::optional<EnergyIssue> worst;
  const auto consider = [&](EnergyIssue issue) {
    if (issue.lu > section_issue_lu && (!worst || issue.lu > worst->lu))
      worst = issue;
  };
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (sections[i].lufs <= silent_lufs)
      continue;
    const bool drop = contains_word(sections[i].name, "drop");
    for (std::size_t j = 0; j < sections.size(); ++j) {
      if (j == i || sections[j].lufs <= silent_lufs)
        continue;
      const bool other_drop = contains_word(sections[j].name, "drop");
      const bool other_break = contains_word(sections[j].name, "break");
      if (drop && other_drop && j > i)
        consider({i, j, sections[i].lufs - sections[j].lufs, false});
      if (drop && other_break)
        consider({i, j, sections[j].lufs - sections[i].lufs, true});
    }
  }
  return worst;
}

// ---- Masking -------------------------------------------------------------------------

namespace {

struct Coactive {
  // Positions in each profile's stored hops, and the hop index, of the hops
  // where both play.
  std::vector<std::size_t> heard_hops;
  std::vector<std::size_t> other_hops;
  std::vector<std::size_t> hops;
  std::array<double, band_count> heard{};
  std::array<double, band_count> other{};
  std::array<bool, band_count> judged{};
};

[[nodiscard]] double hop_total(const BandPowers& bands) noexcept {
  double sum = 0.0;
  for (const float band : bands)
    sum += band;
  return sum;
}

[[nodiscard]] Coactive coactive(const Profile& heard, double heard_gain, const Profile& other,
                                double other_gain) {
  Coactive result;
  const auto loudest = [](const Profile& p) {
    double most = 0.0;
    for (const auto& bands : p.hop_bands)
      most = std::max(most, hop_total(bands));
    return most;
  };
  const double range = std::pow(10.0, -activity_range_db / 10.0);
  const double a_floor = loudest(heard) * range;
  const double b_floor = loudest(other) * range;
  // Both lists are in hop order: walk them together.
  for (std::size_t a = 0, b = 0; a < heard.hop_index.size() && b < other.hop_index.size();) {
    if (heard.hop_index[a] < other.hop_index[b]) {
      ++a;
    } else if (other.hop_index[b] < heard.hop_index[a]) {
      ++b;
    } else {
      if (hop_total(heard.hop_bands[a]) >= a_floor && hop_total(other.hop_bands[b]) >= b_floor) {
        result.heard_hops.push_back(a);
        result.other_hops.push_back(b);
        result.hops.push_back(heard.hop_index[a]);
      }
      ++a;
      ++b;
    }
  }
  // Where the heard track's character lives.
  const auto own = band_powers(heard);
  double strongest = 0.0;
  for (std::size_t band = 0; band < band_count; ++band)
    if (band_centre(band) >= masking_low_hz)
      strongest = std::max(strongest, static_cast<double>(own[band]));
  for (std::size_t band = 0; band < band_count; ++band)
    result.judged[band] = band_centre(band) >= masking_low_hz && own[band] > 0.0F &&
                          power_db(own[band]) >= power_db(strongest) - masking_character_db;
  const double heard_scale = heard_gain * heard_gain;
  const double other_scale = other_gain * other_gain;
  for (std::size_t i = 0; i < result.hops.size(); ++i)
    for (std::size_t band = 0; band < band_count; ++band) {
      result.heard[band] +=
          static_cast<double>(heard.hop_bands[result.heard_hops[i]][band]) * heard_scale;
      result.other[band] +=
          static_cast<double>(other.hop_bands[result.other_hops[i]][band]) * other_scale;
    }
  return result;
}

} // namespace

std::array<double, band_count> masking_loss(const Profile& heard, double heard_gain,
                                            const Profile& other, double other_gain) {
  const auto together = coactive(heard, heard_gain, other, other_gain);
  std::array<double, band_count> loss{};
  for (std::size_t band = 0; band < band_count; ++band)
    if (together.judged[band] && together.heard[band] > 0.0)
      loss[band] = 10.0 * std::log10(1.0 + together.other[band] / together.heard[band]);
  return loss;
}

std::optional<Masking> find_masking(const Profile& heard, double heard_gain, const Profile& other,
                                    double other_gain) {
  const auto together = coactive(heard, heard_gain, other, other_gain);
  const double seconds = static_cast<double>(together.hops.size()) * heard.hop_seconds();
  if (seconds < masking_min_seconds)
    return std::nullopt;
  std::array<double, band_count> loss{};
  for (std::size_t band = 0; band < band_count; ++band)
    if (together.judged[band] && together.heard[band] > 0.0)
      loss[band] = 10.0 * std::log10(1.0 + together.other[band] / together.heard[band]);
  std::optional<Masking> worst;
  for (std::size_t b = 0; b < band_count;) {
    if (loss[b] < masking_threshold_db) {
      ++b;
      continue;
    }
    Masking span{b, b, 0.0, seconds, 0};
    while (span.last_band + 1 < band_count && loss[span.last_band + 1] >= masking_threshold_db)
      ++span.last_band;
    double heard_sum = 0.0;
    double other_sum = 0.0;
    for (auto k = span.first_band; k <= span.last_band; ++k) {
      heard_sum += together.heard[k];
      other_sum += together.other[k];
    }
    span.loss_db = 10.0 * std::log10(1.0 + other_sum / heard_sum);
    // The hop where the other covers the span most.
    double most = -1.0;
    for (std::size_t i = 0; i < together.hops.size(); ++i) {
      double a = 0.0;
      double o = 0.0;
      for (auto k = span.first_band; k <= span.last_band; ++k) {
        a += heard.hop_bands[together.heard_hops[i]][k];
        o += other.hop_bands[together.other_hops[i]][k];
      }
      const double ratio = a > 0.0 ? o / a : 0.0;
      if (ratio > most) {
        most = ratio;
        span.worst_hop = together.hops[i];
      }
    }
    if (!worst || span.loss_db > worst->loss_db)
      worst = span;
    b = span.last_band + 1;
  }
  return worst;
}

// ---- Clean-up ------------------------------------------------------------------------

Balance worst_balance(const Profile& profile) {
  const auto left = band_left(profile);
  const auto right = band_right(profile);
  double loudest = 0.0;
  for (std::size_t b = 0; b < band_count; ++b)
    loudest = std::max({loudest, static_cast<double>(left[b]), static_cast<double>(right[b])});
  Balance worst;
  for (std::size_t b = 0; b < band_count; ++b) {
    const double level = std::max(left[b], right[b]);
    if (!(level > 0.0) || power_db(level) < power_db(loudest) - judged_range_db)
      continue;
    const double difference = power_db(left[b]) - power_db(right[b]);
    if (std::fabs(difference) > std::fabs(worst.db))
      worst = {difference, b};
  }
  return worst;
}

double dc_offset(const Profile& profile) noexcept {
  if (profile.frames == 0)
    return 0.0;
  const auto frames = static_cast<double>(profile.frames);
  return std::max(std::fabs(profile.left_sum), std::fabs(profile.right_sum)) / frames;
}

// ---- Reference -----------------------------------------------------------------------

std::vector<ReferenceDifference> compare_reference(const Profile& mix, double mix_lufs,
                                                   const Profile& reference,
                                                   double reference_lufs) {
  std::vector<ReferenceDifference> result;
  for (const auto& band : reference_bands) {
    const double ours = power_db(power_between(mix, band.low_hz, band.high_hz)) - mix_lufs;
    const double theirs =
        power_db(power_between(reference, band.low_hz, band.high_hz)) - reference_lufs;
    result.push_back({band.name, ours - theirs});
  }
  const auto width = [](const Profile& p) {
    const double nyquist = static_cast<double>(p.sample_rate) / 2.0;
    const double bin_hz = p.fine_bin_hz();
    return power_db(sum_bins(p.side, bin_hz, display_low_hz, nyquist)) -
           power_db(sum_bins(p.mid, bin_hz, display_low_hz, nyquist));
  };
  result.push_back({"Width", width(mix) - width(reference)});
  return result;
}

} // namespace oscillate::analysis::mix
