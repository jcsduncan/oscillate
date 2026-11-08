#include "oscillate/native_dsp/effects/strata.hpp"

#include "dsp/crossover.hpp"
#include "dsp/fft.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "strata_presets.hpp"
#include "strata_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace oscillate::native_dsp::effects::strata {
// The shared primitives (crossovers, FFT); effects::dsp is the effects' own.
namespace base = ::oscillate::native_dsp::dsp;
namespace {

namespace t = strata_table;
using t::band_index;
using t::index_of;

namespace p {
constexpr std::size_t input = index_of("strata.input");
constexpr std::size_t output = index_of("strata.output");
constexpr std::size_t mix = index_of("strata.mix");
constexpr std::size_t auto_gain = index_of("strata.auto_gain");
constexpr std::size_t delta = index_of("strata.delta");
constexpr std::size_t lookahead = index_of("strata.lookahead");
constexpr std::size_t phase = index_of("strata.phase");
constexpr std::size_t mode = index_of("strata.mode");
constexpr std::size_t bands = index_of("strata.bands");
constexpr std::size_t xover_1 = index_of("strata.xover_1");
constexpr std::size_t ott_depth = index_of("strata.ott.depth");
constexpr std::size_t ott_time = index_of("strata.ott.time");
constexpr std::size_t ott_balance = index_of("strata.ott.balance");
constexpr std::size_t learn = index_of("strata.learn");
} // namespace p

static_assert(t::band_count == max_bands);

// Crossovers stay a third of an octave apart and below 0.45 of the rate.
const double crossover_spacing = std::cbrt(2.0);
constexpr double crossover_top = native_dsp::dsp::nyquist_guard;

// OTT: three bands split at 88 Hz and 2.5 kHz,
// each squashed down above and lifted up below its threshold. Depth scales
// both; Up / Down leans it (negative: up only, positive: down only); Time
// runs attack from 1 to 30 ms and release from 40 to 400 ms, slower in the
// low band and quicker in the top.
constexpr std::size_t ott_bands = 3;
constexpr std::array<double, ott_bands - 1> ott_crossovers{88.0, 2'500.0};
constexpr std::array<double, ott_bands> ott_thresholds{-26.0, -28.0, -32.0};
constexpr std::array<double, ott_bands> ott_slowness{1.6, 1.0, 0.7};
constexpr double ott_above_span = 6.0; // 1:1 to 7:1
constexpr double ott_below_span = 3.0; // 1:1 to 4:1
constexpr double ott_attack_ms = 1.0;
constexpr double ott_attack_span_ms = 29.0;
constexpr double ott_release_ms = 40.0;
constexpr double ott_release_span_ms = 360.0;
constexpr double ott_knee_db = 6.0;
constexpr double ott_range_db = 24.0;
constexpr double ott_makeup_db = 1.5; // per unit of downward squash

// Lift never raises near-silence into hiss: it fades in from -84 to -64 dB
// of the band's level (as Nova's Multiband does).
constexpr double lift_floor_db = -84.0;
constexpr double lift_full_db = -64.0;
// The level the lift gate, the display and Learn read: a peak that falls in
// 50 ms.
constexpr double level_release_seconds = 0.05;
// Below this a detector reads silence.
constexpr double silence = 1e-10;
// Solo, mute and bypass glide over 20 ms, so a switch never clicks.
constexpr double toggle_seconds = native_dsp::dsp::switch_fade_seconds;
// Auto gain compares the dry and wet power over 3 s, within +-18 dB.
constexpr double auto_gain_seconds = 3.0;
constexpr double auto_gain_limit_db = 18.0;
constexpr double tiny_power = 1e-12;
// Meters hold their peak for a second.
constexpr double hold_seconds = 1.0;
// Learn: four seconds of each band's level, 0.5 dB bins from -90 dB; the
// suggestion is 4 dB under the level the band exceeds a quarter of the time.
constexpr double learn_seconds = 4.0;
constexpr double learn_floor_db = -90.0;
constexpr double learn_bin_db = 0.5;
constexpr std::size_t learn_bins = 180;
constexpr double learn_percentile = 0.75;
constexpr double learn_offset_db = 4.0;
// The display spectrum: a 2048-point Hann FFT 30 times a second, falling
// 36 dB a second.
constexpr std::size_t spectrum_size = 2'048;
constexpr double spectrum_rate_hz = 30.0;
constexpr double spectrum_fall_db_per_second = 36.0;
constexpr double display_low_hz = 20.0;
constexpr double display_span = 1'000.0;
// The linear-phase bank at 48 kHz (doubled per doubling of the rate): a
// 2047-tap Blackman-windowed sinc per crossover, run by overlap-save on
// 512-frame hops in 4096-point FFTs: 1023 + 512 frames of latency (32 ms).
constexpr double bank_base_rate = 48'000.0;
constexpr std::size_t bank_base_half = 1'023;
constexpr std::size_t bank_base_hop = 512;
constexpr std::size_t bank_base_fft = 4'096;
// The minimum-phase filters' ring after the input stops.
constexpr double tail_seconds = 0.05;

[[nodiscard]] double to_db(double x) noexcept {
  return native_dsp::dsp::gain_to_db(x, native_dsp::dsp::silent_db, silence);
}
[[nodiscard]] bool on(double v) noexcept { return v >= 0.5; }
[[nodiscard]] int whole(double v) noexcept { return static_cast<int>(std::lround(v)); }

[[nodiscard]] Band ott_band(std::span<const double> v, std::size_t b) noexcept {
  const double depth = std::clamp(v[p::ott_depth], 0.0, 1.0);
  const double time = std::clamp(v[p::ott_time], 0.0, 1.0);
  const double balance = std::clamp(v[p::ott_balance], -1.0, 1.0);
  const double up = depth * std::min(1.0, 1.0 - balance);
  const double down = depth * std::min(1.0, 1.0 + balance);
  Band band;
  band.curve = {
      ott_thresholds[b], 1.0 + ott_above_span * down, 1.0 + ott_below_span * up, ott_knee_db,
      ott_range_db,      ott_makeup_db * down};
  band.attack_ms = std::clamp((ott_attack_ms + ott_attack_span_ms * time) * ott_slowness[b],
                              t::attack_min, t::attack_max);
  band.release_ms = std::clamp((ott_release_ms + ott_release_span_ms * time) * ott_slowness[b],
                               t::release_min, t::release_max);
  return band;
}

// ---- The linear-phase bank -----------------------------------------------------------

// Splits stereo into bands whose FIRs (differences of Blackman-windowed sinc
// low-passes) sum to a pure delay of half_length frames. Left and right run
// as one complex signal through one FFT per hop, and every band's spectrum
// is real-symmetric, so each band needs one inverse transform. Output is a
// further hop late: the bank's latency is half_length + hop.
class LinearBank {
public:
  LinearBank(const Frames& f, double rate)
      : rate_(rate), half_(f.half_length), hop_(f.hop), size_(f.fft), fft_(f.fft), history_(f.fft),
        work_(f.fft), lowpass_(f.fft), previous_(f.fft) {
    for (auto& s : spectra_) {
      s.assign(size_, {});
    }
    for (auto& o : out_) {
      o.assign(hop_, {});
    }
    taps_.assign(2 * half_ + 1, 0.0);
  }

  void set(const Layout& layout) noexcept {
    if (layout.bands != layout_.bands ||
        !std::equal(layout.crossovers.begin(), layout.crossovers.begin() + (layout.bands - 1),
                    layout_.crossovers.begin())) {
      layout_ = layout;
      dirty_ = true;
    }
  }
  void reset() noexcept {
    std::ranges::fill(history_, std::complex<double>{});
    for (auto& o : out_) {
      std::ranges::fill(o, std::complex<double>{});
    }
    position_ = 0;
  }
  [[nodiscard]] std::size_t bands() const noexcept { return layout_.bands; }

  // Writes bands() pairs (the bank's output now) and takes one input frame.
  void process(double left, double right, std::span<double> out_left,
               std::span<double> out_right) noexcept {
    for (std::size_t k = 0; k < layout_.bands; ++k) {
      out_left[k] = out_[k][position_].real();
      out_right[k] = out_[k][position_].imag();
    }
    history_[size_ - hop_ + position_] = {left, right};
    if (++position_ == hop_) {
      run();
      position_ = 0;
    }
  }

private:
  void run() noexcept {
    if (dirty_) {
      design();
    }
    std::ranges::copy(history_, work_.begin());
    fft_.forward(work_);
    for (std::size_t k = 0; k < layout_.bands; ++k) {
      for (std::size_t i = 0; i < size_; ++i) {
        lowpass_[i] = work_[i] * spectra_[k][i];
      }
      fft_.inverse(lowpass_);
      std::copy(lowpass_.begin() + static_cast<std::ptrdiff_t>(size_ - hop_), lowpass_.end(),
                out_[k].begin());
    }
    std::copy(history_.begin() + static_cast<std::ptrdiff_t>(hop_), history_.end(),
              history_.begin());
  }

  // Band k = lowpass(k) - lowpass(k - 1); the first band is lowpass(0) and
  // the last the delay less lowpass(n - 2), so the bands sum to the delay.
  void design() noexcept {
    dirty_ = false;
    const std::size_t n = layout_.bands;
    const auto length = static_cast<double>(taps_.size() - 1);
    for (std::size_t i = 0; i < size_; ++i) {
      previous_[i] = {};
    }
    for (std::size_t k = 0; k + 1 < n; ++k) {
      const double cutoff = 2.0 * layout_.crossovers[k] / rate_;
      double sum = 0.0;
      for (std::size_t m = 0; m < taps_.size(); ++m) {
        const double x = static_cast<double>(m) - static_cast<double>(half_);
        const double sinc =
            x == 0.0 ? 1.0
                     : std::sin(std::numbers::pi * cutoff * x) / (std::numbers::pi * cutoff * x);
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(m) / length;
        const double blackman = 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
        taps_[m] = cutoff * sinc * blackman;
        sum += taps_[m];
      }
      for (std::size_t i = 0; i < size_; ++i) {
        lowpass_[i] =
            i < taps_.size() ? std::complex<double>{taps_[i] / sum, 0.0} : std::complex<double>{};
      }
      fft_.forward(lowpass_);
      for (std::size_t i = 0; i < size_; ++i) {
        spectra_[k][i] = lowpass_[i] - previous_[i];
        previous_[i] = lowpass_[i];
      }
    }
    for (std::size_t i = 0; i < size_; ++i) {
      const double angle = -2.0 * std::numbers::pi * static_cast<double>(i) *
                           static_cast<double>(half_) / static_cast<double>(size_);
      spectra_[n - 1][i] = std::polar(1.0, angle) - previous_[i];
    }
  }

  double rate_;
  std::size_t half_;
  std::size_t hop_;
  std::size_t size_;
  base::Fft fft_;
  std::vector<std::complex<double>> history_;
  std::vector<std::complex<double>> work_;
  std::vector<std::complex<double>> lowpass_;
  std::vector<std::complex<double>> previous_;
  std::vector<double> taps_;
  std::array<std::vector<std::complex<double>>, max_bands> spectra_{};
  std::array<std::vector<std::complex<double>>, max_bands> out_{};
  Layout layout_{};
  std::size_t position_ = 0;
  bool dirty_ = true;
};

// ---- One band's dynamics -------------------------------------------------------------

struct Slew {
  double value = 0.0;
  double target = 0.0;
  double step = 1.0;
  void to(double goal, bool jump) noexcept {
    target = goal;
    if (jump) {
      value = goal;
    }
  }
  double next() noexcept { return value += std::clamp(target - value, -step, step); }
};

struct BandState {
  Curve curve{};
  double attack = 0.0;
  double release = 0.0;
  Process process = Process::stereo;
  Key key = Key::band;
  double gain_db = 0.0; // smoothed change, makeup excluded
  double level = 0.0;   // falling peak of the detector
  Slew weight;          // solo and mute
  Slew bypass;
  std::array<dsp::Delay, 2> lookahead{};
  std::array<dsp::Delay, 2> sidechain_align{};
  std::array<std::uint32_t, learn_bins> histogram{};
};

[[nodiscard]] double detect(Process process, double l, double r) noexcept {
  switch (process) {
  case Process::mid:
    return std::fabs(0.5 * (l + r));
  case Process::side:
    return std::fabs(0.5 * (l - r));
  case Process::left:
    return std::fabs(l);
  case Process::right:
    return std::fabs(r);
  case Process::stereo:
    break;
  }
  return std::max(std::fabs(l), std::fabs(r));
}

void apply(Process process, double gain, double& l, double& r) noexcept {
  switch (process) {
  case Process::stereo:
    l *= gain;
    r *= gain;
    return;
  case Process::mid:
  case Process::side: {
    double m = 0.5 * (l + r);
    double s = 0.5 * (l - r);
    (process == Process::mid ? m : s) *= gain;
    l = m + s;
    r = m - s;
    return;
  }
  case Process::left:
    l *= gain;
    return;
  case Process::right:
    r *= gain;
    return;
  }
}

class StrataKernel final : public EffectKernel {
public:
  StrataKernel(std::span<const double> values, double sample_rate)
      : rate_(sample_rate), frames_(frames_for(values, sample_rate)),
        linear_(whole(values[p::phase]) == t::Phase::linear),
        level_decay_(std::exp(-1.0 / (level_release_seconds * sample_rate))),
        hold_(std::exp(std::log(1e-3) / (hold_seconds * sample_rate))),
        auto_gain_coefficient_(1.0 - std::exp(-1.0 / (auto_gain_seconds * sample_rate))),
        learn_target_(static_cast<std::uint64_t>(std::ceil(learn_seconds * sample_rate))),
        spectrum_interval_(static_cast<std::size_t>(std::lround(sample_rate / spectrum_rate_hz))),
        fft_(spectrum_size), spectrum_work_(spectrum_size), spectrum_input_(spectrum_size),
        spectrum_output_(spectrum_size), window_(spectrum_size) {
    if (linear_) {
      bank_ = std::make_unique<LinearBank>(frames_, sample_rate);
    }
    const std::size_t align = linear_ ? frames_.half_length + frames_.hop : 0;
    const double toggle_step = 1.0 / (toggle_seconds * sample_rate);
    for (auto& band : bands_) {
      for (auto& line : band.lookahead) {
        line.prepare(frames_.lookahead + 1);
      }
      for (auto& line : band.sidechain_align) {
        line.prepare(align + 1);
      }
      band.weight.step = toggle_step;
      band.bypass.step = toggle_step;
    }
    for (auto& line : input_align_) {
      line.prepare(align + 1);
    }
    align_ = align;
    for (std::size_t i = 0; i < spectrum_size; ++i) {
      window_[i] = oscillate::native_dsp::dsp::hann(static_cast<double>(i),
                                                    static_cast<double>(spectrum_size));
    }
    const double bin_hz = sample_rate / static_cast<double>(spectrum_size);
    const auto top = static_cast<double>(spectrum_size) / 2.0;
    for (std::size_t i = 0; i < display_bands; ++i) {
      const auto n = static_cast<double>(display_bands);
      const double lo = display_low_hz * std::pow(display_span, static_cast<double>(i) / n);
      const double hi = display_low_hz * std::pow(display_span, static_cast<double>(i + 1) / n);
      const double centre =
          display_low_hz * std::pow(display_span, (static_cast<double>(i) + 0.5) / n);
      auto first = std::clamp(std::ceil(lo / bin_hz), 1.0, top);
      auto last = std::clamp(std::floor(hi / bin_hz), 1.0, top);
      if (first > last) {
        first = last = std::clamp(std::round(centre / bin_hz), 1.0, top);
      }
      first_bin_[i] = static_cast<std::size_t>(first);
      last_bin_[i] = static_cast<std::size_t>(last);
    }
    input_spectrum_.fill(silent_db);
    output_spectrum_.fill(silent_db);
    learned_.fill(silent_db);
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport&) noexcept override {
    const bool jump = !primed_;
    const double steps = std::max<double>(1.0, frames);
    layout_ = layout_for(v, rate_);
    std::array<base::Lr4Design, max_crossovers> designs{};
    for (std::size_t k = 0; k + 1 < layout_.bands; ++k) {
      designs[k] = base::Lr4Design::at(layout_.crossovers[k], rate_);
    }
    for (auto& tree : trees_) {
      tree.set(designs, layout_.bands - 1);
    }
    for (auto& tree : sidechain_trees_) {
      tree.set(designs, layout_.bands - 1);
    }
    if (bank_) {
      bank_->set(layout_);
    }
    bool any_solo = false;
    std::array<Band, max_bands> settings{};
    for (std::size_t b = 0; b < layout_.bands; ++b) {
      settings[b] = band_for(v, b);
      any_solo = any_solo || settings[b].solo;
    }
    for (std::size_t b = 0; b < max_bands; ++b) {
      auto& state = bands_[b];
      const auto& s = settings[b];
      state.curve = s.curve;
      state.attack = std::exp(-1.0 / (s.attack_ms * 1e-3 * rate_));
      state.release = std::exp(-1.0 / (s.release_ms * 1e-3 * rate_));
      state.process = s.process;
      state.key = s.key;
      const bool heard = b < layout_.bands && (any_solo ? s.solo : !s.mute);
      state.weight.to(heard ? 1.0 : 0.0, jump);
      state.bypass.to(s.bypass ? 1.0 : 0.0, jump);
    }
    input_gain_.to(dsp::db_to_gain(v[p::input]), steps, jump);
    output_gain_.to(dsp::db_to_gain(v[p::output]), steps, jump);
    mix_.to(std::clamp(v[p::mix], 0.0, 1.0), steps, jump);
    auto_gain_ = on(v[p::auto_gain]);
    delta_ = on(v[p::delta]);
    const bool learn = on(v[p::learn]);
    if (learn && !learning_) {
      for (auto& band : bands_) {
        band.histogram.fill(0);
      }
      learn_frames_ = 0;
      learned_.fill(silent_db);
    } else if (!learn) {
      learn_frames_ = 0;
      learned_.fill(silent_db);
    }
    learning_ = learn;
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    keyed_ = !sidechain.empty();
    const std::size_t n = layout_.bands;
    bool keyed_bands = false;
    for (std::size_t b = 0; b < n; ++b) {
      keyed_bands = keyed_bands || bands_[b].key == Key::sidechain;
    }
    const bool split_sidechain = keyed_ && keyed_bands;
    std::array<double, max_bands> left{};
    std::array<double, max_bands> right{};
    std::array<double, max_bands> key_left{};
    std::array<double, max_bands> key_right{};
    for (std::size_t i = 0; i < audio.size(); ++i) {
      auto& frame = audio[i];
      const double gain_in = input_gain_.next();
      const double xl = std::isfinite(frame.left) ? frame.left * gain_in : 0.0;
      const double xr = std::isfinite(frame.right) ? frame.right * gain_in : 0.0;
      meter(input_peak_, xl, xr);
      if (bank_) {
        bank_->process(xl, xr, left, right);
      } else {
        trees_[0].process(xl, left);
        trees_[1].process(xr, right);
      }
      if (split_sidechain) {
        const double sl = sidechain[i].left;
        const double sr = sidechain[i].right;
        sidechain_trees_[0].process(std::isfinite(sl) ? sl : 0.0, key_left);
        sidechain_trees_[1].process(std::isfinite(sr) ? sr : 0.0, key_right);
      }
      input_align_[0].write(xl);
      input_align_[1].write(xr);
      const double input_left = input_align_[0].sample(align_);
      const double input_right = input_align_[1].sample(align_);

      double dry_l = 0.0;
      double dry_r = 0.0;
      double wet_l = 0.0;
      double wet_r = 0.0;
      double delta_l = 0.0;
      double delta_r = 0.0;
      const bool learning = learning_ && learn_frames_ < learn_target_;
      for (std::size_t b = 0; b < n; ++b) {
        auto& band = bands_[b];
        double kl = left[b];
        double kr = right[b];
        if (band.key == Key::input) {
          kl = input_left;
          kr = input_right;
        } else if (band.key == Key::sidechain && split_sidechain) {
          band.sidechain_align[0].write(key_left[b]);
          band.sidechain_align[1].write(key_right[b]);
          kl = band.sidechain_align[0].sample(align_);
          kr = band.sidechain_align[1].sample(align_);
        }
        const double d = detect(band.process, kl, kr);
        band.level = std::max(d, band.level * level_decay_);
        const double level_db = to_db(d);
        const double envelope_db = to_db(band.level);
        double target = gain_change_db(level_db, band.curve);
        if (target > 0.0) {
          target *=
              std::clamp((envelope_db - lift_floor_db) / (lift_full_db - lift_floor_db), 0.0, 1.0);
        }
        const double coefficient = target < band.gain_db ? band.attack : band.release;
        band.gain_db = target + coefficient * (band.gain_db - target);
        if (learning && envelope_db > learn_floor_db) {
          const auto bin =
              std::min(learn_bins - 1,
                       static_cast<std::size_t>((envelope_db - learn_floor_db) / learn_bin_db));
          ++band.histogram[bin];
        }

        band.lookahead[0].write(left[b]);
        band.lookahead[1].write(right[b]);
        const double al = band.lookahead[0].sample(frames_.lookahead);
        const double ar = band.lookahead[1].sample(frames_.lookahead);
        const double bypass = band.bypass.next();
        const double gain = dsp::db_to_gain(band.gain_db + band.curve.makeup_db);
        double pl = al;
        double pr = ar;
        apply(band.process, 1.0 + (gain - 1.0) * (1.0 - bypass), pl, pr);
        const double weight = band.weight.next();
        dry_l += al;
        dry_r += ar;
        wet_l += weight * pl;
        wet_r += weight * pr;
        delta_l += weight * (al - pl);
        delta_r += weight * (ar - pr);
      }
      for (std::size_t b = n; b < max_bands; ++b) {
        bands_[b].gain_db = 0.0;
        bands_[b].level = 0.0;
      }
      if (learning) {
        ++learn_frames_;
        if (learn_frames_ == learn_target_) {
          suggest();
        }
      }

      double gain_auto = 1.0;
      if (auto_gain_ && !delta_) {
        dry_power_ += (dry_l * dry_l + dry_r * dry_r - dry_power_) * auto_gain_coefficient_;
        wet_power_ += (wet_l * wet_l + wet_r * wet_r - wet_power_) * auto_gain_coefficient_;
        auto_gain_db_ =
            std::clamp(10.0 * std::log10((dry_power_ + tiny_power) / (wet_power_ + tiny_power)),
                       -auto_gain_limit_db, auto_gain_limit_db);
        gain_auto = dsp::db_to_gain(auto_gain_db_);
      } else {
        auto_gain_db_ = 0.0;
      }
      const double m = mix_.next();
      const double gain_out = output_gain_.next();
      double out_l = delta_ ? delta_l : dry_l * (1.0 - m) + wet_l * gain_auto * m;
      double out_r = delta_ ? delta_r : dry_r * (1.0 - m) + wet_r * gain_auto * m;
      out_l *= gain_out;
      out_r *= gain_out;
      frame.left = std::isfinite(out_l) ? static_cast<float>(out_l) : 0.0F;
      frame.right = std::isfinite(out_r) ? static_cast<float>(out_r) : 0.0F;
      meter(output_peak_, out_l, out_r);
      spectrum_input_[spectrum_write_] = 0.5 * (xl + xr);
      spectrum_output_[spectrum_write_] = 0.5 * (out_l + out_r);
      spectrum_write_ = (spectrum_write_ + 1) % spectrum_size;
      if (++spectrum_count_ >= spectrum_interval_) {
        spectrum_count_ = 0;
        analyse();
      }
    }
  }

  void reset() noexcept override {
    for (auto& tree : trees_) {
      tree.reset();
    }
    for (auto& tree : sidechain_trees_) {
      tree.reset();
    }
    if (bank_) {
      bank_->reset();
    }
    for (auto& band : bands_) {
      band.gain_db = 0.0;
      band.level = 0.0;
      for (auto* line : {&band.lookahead[0], &band.lookahead[1], &band.sidechain_align[0],
                         &band.sidechain_align[1]}) {
        line->reset();
      }
    }
    for (auto& line : input_align_) {
      line.reset();
    }
    input_peak_ = {};
    output_peak_ = {};
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    namespace tm = telemetry;
    if (out.size() < tm::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[tm::input_peak + c] = static_cast<float>(to_db(input_peak_[c]));
      out[tm::output_peak + c] = static_cast<float>(to_db(output_peak_[c]));
    }
    out[tm::keyed] = keyed_ ? 1.0F : 0.0F;
    out[tm::latency_ms] = static_cast<float>(1e3 * frames_.latency / rate_);
    out[tm::auto_gain_db] = static_cast<float>(auto_gain_db_);
    out[tm::learn_progress] = learning_ ? static_cast<float>(static_cast<double>(learn_frames_) /
                                                             static_cast<double>(learn_target_))
                                        : 0.0F;
    out[tm::band_count] = static_cast<float>(layout_.bands);
    for (std::size_t b = 0; b < max_bands; ++b) {
      const bool active = b < layout_.bands;
      out[tm::band_gain + b] = active ? static_cast<float>(bands_[b].gain_db) : 0.0F;
      out[tm::band_level + b] = active ? static_cast<float>(to_db(bands_[b].level)) : silent_db;
      out[tm::learned + b] = learned_[b];
    }
    for (std::size_t k = 0; k < max_crossovers; ++k) {
      out[tm::crossovers + k] =
          k + 1 < layout_.bands ? static_cast<float>(layout_.crossovers[k]) : 0.0F;
    }
    for (std::size_t i = 0; i < display_bands; ++i) {
      out[tm::input_spectrum + i] = input_spectrum_[i];
      out[tm::output_spectrum + i] = output_spectrum_[i];
    }
    return tm::size;
  }

private:
  void meter(std::array<double, 2>& peak, double l, double r) const noexcept {
    peak[0] = std::max(peak[0] * hold_, std::fabs(l));
    peak[1] = std::max(peak[1] * hold_, std::fabs(r));
  }

  // Each band's level at the percentile, less the offset, as its threshold.
  void suggest() noexcept {
    for (std::size_t b = 0; b < max_bands; ++b) {
      std::uint64_t total = 0;
      for (const auto count : bands_[b].histogram) {
        total += count;
      }
      if (b >= layout_.bands || total == 0) {
        learned_[b] = silent_db;
        continue;
      }
      const auto wanted = static_cast<std::uint64_t>(learn_percentile * static_cast<double>(total));
      std::uint64_t seen = 0;
      std::size_t bin = 0;
      for (; bin < learn_bins; ++bin) {
        seen += bands_[b].histogram[bin];
        if (seen > wanted) {
          break;
        }
      }
      const double level = learn_floor_db + (static_cast<double>(bin) + 0.5) * learn_bin_db;
      learned_[b] = static_cast<float>(
          std::clamp(level - learn_offset_db, t::threshold_min, t::threshold_max));
    }
  }

  // The input and output spectrum in one transform: input real, output
  // imaginary, separated by conjugate symmetry.
  void analyse() noexcept {
    for (std::size_t i = 0; i < spectrum_size; ++i) {
      const std::size_t at = (spectrum_write_ + i) % spectrum_size;
      spectrum_work_[i] = {spectrum_input_[at] * window_[i], spectrum_output_[at] * window_[i]};
    }
    fft_.forward(spectrum_work_);
    // A full-scale sine reads 0 dB: the Hann window halves the amplitude.
    const double scale = 4.0 / static_cast<double>(spectrum_size);
    const double fall =
        spectrum_fall_db_per_second * static_cast<double>(spectrum_interval_) / rate_;
    for (std::size_t i = 0; i < display_bands; ++i) {
      double in = 0.0;
      double out = 0.0;
      for (std::size_t bin = first_bin_[i]; bin <= last_bin_[i]; ++bin) {
        const auto z = spectrum_work_[bin];
        const auto mirror = std::conj(spectrum_work_[(spectrum_size - bin) % spectrum_size]);
        in = std::max(in, std::abs(0.5 * (z + mirror)));
        out = std::max(out, std::abs(0.5 * (z - mirror)));
      }
      const auto fresh_in = static_cast<float>(to_db(in * scale));
      const auto fresh_out = static_cast<float>(to_db(out * scale));
      input_spectrum_[i] = std::max(fresh_in, static_cast<float>(input_spectrum_[i] - fall));
      output_spectrum_[i] = std::max(fresh_out, static_cast<float>(output_spectrum_[i] - fall));
    }
  }

  double rate_;
  Frames frames_;
  bool linear_;
  double level_decay_;
  double hold_;
  double auto_gain_coefficient_;
  std::uint64_t learn_target_;
  std::size_t spectrum_interval_;
  std::size_t align_ = 0;
  Layout layout_{};
  std::array<base::CrossoverTree<max_crossovers>, 2> trees_{};
  std::array<base::CrossoverTree<max_crossovers>, 2> sidechain_trees_{};
  std::unique_ptr<LinearBank> bank_;
  std::array<BandState, max_bands> bands_{};
  std::array<dsp::Delay, 2> input_align_{};
  dsp::Ramp input_gain_;
  dsp::Ramp output_gain_;
  dsp::Ramp mix_;
  bool auto_gain_ = false;
  bool delta_ = false;
  bool learning_ = false;
  bool keyed_ = false;
  bool primed_ = false;
  double dry_power_ = 0.0;
  double wet_power_ = 0.0;
  double auto_gain_db_ = 0.0;
  std::uint64_t learn_frames_ = 0;
  std::array<float, max_bands> learned_{};
  std::array<double, 2> input_peak_{};
  std::array<double, 2> output_peak_{};
  base::Fft fft_;
  std::vector<std::complex<double>> spectrum_work_;
  std::vector<double> spectrum_input_;
  std::vector<double> spectrum_output_;
  std::vector<double> window_;
  std::size_t spectrum_write_ = 0;
  std::size_t spectrum_count_ = 0;
  std::array<std::size_t, display_bands> first_bin_{};
  std::array<std::size_t, display_bands> last_bin_{};
  std::array<float, display_bands> input_spectrum_{};
  std::array<float, display_bands> output_spectrum_{};
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (values.size() != t::parameters.size() || !(sample_rate > 0.0)) {
    return nullptr;
  }
  return std::make_unique<StrataKernel>(values, sample_rate);
}

// The linear-phase FIR's second half rings after the latency, and the
// minimum-phase filters briefly.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  const auto f = frames_for(values, sample_rate);
  const bool linear = whole(values[p::phase]) == t::Phase::linear;
  return (linear ? f.half_length : 0) +
         static_cast<std::uint64_t>(std::ceil(tail_seconds * sample_rate));
}

std::uint32_t latency_frames(std::span<const double> values, double sample_rate) {
  return frames_for(values, sample_rate).latency;
}

const EngineDescriptor descriptor{"strata", "Strata", 1, t::parameters, strata_presets::presets};
const EffectEngine strata_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true};

} // namespace

double gain_change_db(double level_db, const Curve& curve) noexcept {
  const double threshold = curve.threshold_db;
  const double above = 1.0 / curve.above;
  const double below = 1.0 / curve.below;
  const double knee = curve.knee_db;
  const double low = threshold - 0.5 * knee;
  double out = 0.0;
  if (knee > 0.0 && level_db > low && level_db < threshold + 0.5 * knee) {
    const double d = level_db - low;
    out = threshold - 0.5 * knee * below + d * below + (above - below) * d * d / (2.0 * knee);
  } else if (level_db > threshold) {
    out = threshold + (level_db - threshold) * above;
  } else {
    out = threshold + (level_db - threshold) * below;
  }
  return std::clamp(out - level_db, -curve.range_db, curve.range_db);
}

Layout layout_for(std::span<const double> v, double sample_rate) noexcept {
  Layout layout;
  layout.ott = whole(v[p::mode]) == t::Mode::ott_mode;
  std::array<double, max_crossovers> wanted{};
  if (layout.ott) {
    layout.bands = ott_bands;
    std::ranges::copy(ott_crossovers, wanted.begin());
  } else {
    layout.bands = static_cast<std::size_t>(
                       std::clamp(whole(v[p::bands]), 0, static_cast<int>(max_bands) - 1)) +
                   1;
    for (std::size_t k = 0; k < max_crossovers; ++k) {
      wanted[k] = v[p::xover_1 + k];
    }
  }
  const double top = std::min(t::crossover_max, crossover_top * sample_rate);
  const std::size_t count = layout.bands - 1;
  for (std::size_t k = 0; k < count; ++k) {
    const double room = std::pow(crossover_spacing, static_cast<double>(count - 1 - k));
    const double floor = k == 0 ? t::crossover_min : layout.crossovers[k - 1] * crossover_spacing;
    layout.crossovers[k] = std::clamp(wanted[k], floor, std::max(floor, top / room));
  }
  return layout;
}

Band band_for(std::span<const double> v, std::size_t b) noexcept {
  Band band;
  if (whole(v[p::mode]) == t::Mode::ott_mode && b < ott_bands) {
    band = ott_band(v, b);
  } else {
    namespace f = t::band;
    band.curve = {v[band_index(b, f::threshold)], v[band_index(b, f::above)],
                  v[band_index(b, f::below)],     v[band_index(b, f::knee)],
                  v[band_index(b, f::range)],     v[band_index(b, f::makeup)]};
    band.attack_ms = v[band_index(b, f::attack)];
    band.release_ms = v[band_index(b, f::release)];
    band.process = static_cast<Process>(
        std::clamp(whole(v[band_index(b, f::process)]), 0, static_cast<int>(t::Process::right)));
    band.key = static_cast<Key>(
        std::clamp(whole(v[band_index(b, f::key)]), 0, static_cast<int>(t::Key::sidechain_key)));
  }
  namespace f = t::band;
  band.solo = on(v[band_index(b, f::solo)]);
  band.mute = on(v[band_index(b, f::mute)]);
  band.bypass = on(v[band_index(b, f::bypass)]);
  return band;
}

std::vector<NamedValue> ott_values(std::span<const double> v) {
  std::vector<NamedValue> out;
  const auto id = [](std::size_t index) { return std::string(t::parameters[index].id); };
  out.push_back({id(p::mode), static_cast<double>(t::Mode::bands_mode)});
  out.push_back({id(p::bands), static_cast<double>(ott_bands - 1)});
  for (std::size_t k = 0; k + 1 < ott_bands; ++k) {
    out.push_back({id(p::xover_1 + k), ott_crossovers[k]});
  }
  namespace f = t::band;
  for (std::size_t b = 0; b < ott_bands; ++b) {
    const Band band = ott_band(v, b);
    out.push_back({id(band_index(b, f::threshold)), band.curve.threshold_db});
    out.push_back({id(band_index(b, f::above)), band.curve.above});
    out.push_back({id(band_index(b, f::below)), band.curve.below});
    out.push_back({id(band_index(b, f::knee)), band.curve.knee_db});
    out.push_back({id(band_index(b, f::range)), band.curve.range_db});
    out.push_back({id(band_index(b, f::attack)), band.attack_ms});
    out.push_back({id(band_index(b, f::release)), band.release_ms});
    out.push_back({id(band_index(b, f::makeup)), band.curve.makeup_db});
    out.push_back({id(band_index(b, f::process)), static_cast<double>(t::Process::stereo)});
    out.push_back({id(band_index(b, f::key)), static_cast<double>(t::Key::band_key)});
  }
  return out;
}

double band_magnitude(const Layout& layout, std::size_t band, double hz,
                      double sample_rate) noexcept {
  std::array<base::Lr4Design, max_crossovers> designs{};
  const std::size_t count = layout.bands - 1;
  for (std::size_t k = 0; k < count; ++k) {
    designs[k] = base::Lr4Design::at(layout.crossovers[k], sample_rate);
  }
  return base::tree_band_magnitude(designs, count, band, hz, sample_rate);
}

Frames frames_for(std::span<const double> v, double sample_rate) noexcept {
  std::size_t scale = 1;
  while (static_cast<double>(scale) * bank_base_rate * 1.5 < sample_rate) {
    scale *= 2;
  }
  Frames f;
  f.half_length = bank_base_half * scale;
  f.hop = bank_base_hop * scale;
  f.fft = bank_base_fft * scale;
  const double lookahead_ms = std::clamp(v[p::lookahead], 0.0, t::lookahead_max_ms);
  f.lookahead = static_cast<std::uint32_t>(std::lround(lookahead_ms * 1e-3 * sample_rate));
  const bool linear = whole(v[p::phase]) == t::Phase::linear;
  f.latency = f.lookahead + (linear ? static_cast<std::uint32_t>(f.half_length + f.hop) : 0U);
  return f;
}

const EffectEngine& engine() { return strata_engine; }

} // namespace oscillate::native_dsp::effects::strata
