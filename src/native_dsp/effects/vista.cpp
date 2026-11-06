#include "oscillate/native_dsp/effects/vista.hpp"

#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "vista_presets.hpp"
#include "vista_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::effects::vista {
namespace {

using vista_table::index_of;

namespace p {
constexpr std::size_t bands = index_of("vista.bands");
constexpr std::size_t xover_1 = index_of("vista.xover_1");
constexpr std::size_t xover_2 = index_of("vista.xover_2");
constexpr std::size_t xover_3 = index_of("vista.xover_3");
constexpr std::size_t width_1 = index_of("vista.width_1");
constexpr std::size_t width = index_of("vista.width");
constexpr std::size_t mono_bass = index_of("vista.mono_bass");
constexpr std::size_t mono_freq = index_of("vista.mono_freq");
constexpr std::size_t stereoize = index_of("vista.stereoize");
constexpr std::size_t stereoize_mode = index_of("vista.stereoize_mode");
constexpr std::size_t haas_time = index_of("vista.haas_time");
constexpr std::size_t safe = index_of("vista.safe");
constexpr std::size_t safe_floor = index_of("vista.safe_floor");
constexpr std::size_t rotation = index_of("vista.rotation");
constexpr std::size_t balance = index_of("vista.balance");
constexpr std::size_t swap = index_of("vista.swap");
constexpr std::size_t invert = index_of("vista.invert");
constexpr std::size_t output = index_of("vista.output");
} // namespace p

// Crossovers stay a third of an octave apart and below 0.45 fs.
constexpr double crossover_spacing = 1.26;
// Stereoize works above 300 Hz, so bass stays mono; the diffuse
// decorrelator is six allpasses of mutually prime lengths.
constexpr double stereoize_low_hz = 300.0;
constexpr std::array<double, 6> decorrelator_ms{1.31, 2.87, 4.13, 6.71, 9.07, 13.27};
constexpr double decorrelator_g = 0.6;
// Amount a sets the synthesised side to sqrt(a / (2 - a)) of the mid, so a
// mono source's correlation (1 - g^2) / (1 + g^2) falls evenly to 0 at 100%.
[[nodiscard]] double stereoize_gain(double amount) noexcept {
  return std::sqrt(amount / (2.0 - amount));
}
constexpr double max_haas_seconds = 0.035;
// Safe width: energies over 50 ms; its gain falls in 5 ms, recovers in 150.
constexpr double safe_window_seconds = 0.05;
constexpr double safe_attack_seconds = 0.005;
constexpr double safe_release_seconds = 0.15;
constexpr double meter_seconds = 0.3;
constexpr std::size_t scope_decimation = 4;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }

// Toggles (swap, polarity, mono bass) glide over 20 ms, so a switch is a
// crossfade, never a step.
constexpr double toggle_seconds = native_dsp::dsp::switch_fade_seconds;
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

// A fourth-order Linkwitz-Riley split: two Butterworth sections each way.
// Low plus high is a second-order allpass, the same for every input.
struct Split {
  synth::MultimodeFilter low_a;
  synth::MultimodeFilter low_b;
  synth::MultimodeFilter high_a;
  synth::MultimodeFilter high_b;
  void reset() noexcept {
    low_a.reset();
    low_b.reset();
    high_a.reset();
    high_b.reset();
  }
  void process(double x, const synth::FilterCoefficients& lp, const synth::FilterCoefficients& hp,
               double& low, double& high) noexcept {
    low = low_b.process(low_a.process(x, lp), lp);
    high = high_b.process(high_a.process(x, hp), hp);
  }
};

struct Coefficients {
  synth::FilterCoefficients lp{};
  synth::FilterCoefficients hp{};
  void set(double hz, double rate) noexcept {
    lp = synth::make_filter_coefficients(synth::FilterMode::lowpass_12, hz / rate, 0.0, 1.0);
    hp = synth::make_filter_coefficients(synth::FilterMode::highpass_12, hz / rate, 0.0, 1.0);
  }
};

// The mono-bass split and the three-crossover band tree, phase-compensated
// so every band carries the same total allpass: mid and side each get one.
struct Tree {
  Split mono;
  Split middle;    // At crossover 2.
  Split low_comp;  // Crossover 3's allpass, for the two low bands.
  Split low;       // At crossover 1.
  Split high_comp; // Crossover 1's allpass, for the two high bands.
  Split high;      // At crossover 3.
  void reset() noexcept {
    for (auto* split : {&mono, &middle, &low_comp, &low, &high_comp, &high}) {
      split->reset();
    }
  }
};

class VistaKernel final : public EffectKernel {
public:
  explicit VistaKernel(double sample_rate)
      : rate_(sample_rate),
        safe_window_(1.0 - std::exp(-1.0 / (safe_window_seconds * sample_rate))),
        safe_attack_(1.0 - std::exp(-1.0 / (safe_attack_seconds * sample_rate))),
        safe_release_(1.0 - std::exp(-1.0 / (safe_release_seconds * sample_rate))),
        meter_(1.0 - std::exp(-1.0 / (meter_seconds * sample_rate))) {
    for (std::size_t k = 0; k < decorrelator_.size(); ++k) {
      decorrelator_frames_[k] = std::max<std::size_t>(
          2, static_cast<std::size_t>(std::lround(decorrelator_ms[k] * 1e-3 * sample_rate)));
      decorrelator_[k].prepare(decorrelator_frames_[k] + 8);
    }
    haas_.prepare(static_cast<std::size_t>(std::ceil(max_haas_seconds * sample_rate)) + 8);
    stereoize_low_.set(stereoize_low_hz, sample_rate);
    const double toggle_step = 1.0 / (toggle_seconds * sample_rate);
    mono_amount_.step = toggle_step;
    swap_.step = toggle_step;
    // Polarity runs from +1 to -1: twice the distance.
    invert_left_.step = 2.0 * toggle_step;
    invert_right_.step = 2.0 * toggle_step;
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const SynthTransport&) noexcept override {
    const double top = native_dsp::dsp::nyquist_guard * rate_;
    const double f1 = std::min(v[p::xover_1], top / (crossover_spacing * crossover_spacing));
    const double f2 =
        std::min(std::max(v[p::xover_2], f1 * crossover_spacing), top / crossover_spacing);
    const double f3 = std::min(std::max(v[p::xover_3], f2 * crossover_spacing), top);
    crossover_[0].set(f1, rate_);
    crossover_[1].set(f2, rate_);
    crossover_[2].set(f3, rate_);
    mono_.set(v[p::mono_freq], rate_);

    const double steps = std::max<double>(1.0, frames);
    const bool jump = !primed_;
    const auto count = static_cast<std::size_t>(as_int(v[p::bands])) + 1;
    for (std::size_t j = 0; j < bands; ++j) {
      // Tree band j takes the width of user band min(j, count - 1).
      const double w = v[p::width_1 + std::min(j, count - 1)] * v[p::width];
      width_[j].to(w, steps, jump);
    }
    mono_amount_.to(v[p::mono_bass] != 0.0 ? 1.0 : 0.0, jump);
    const bool haas = as_int(v[p::stereoize_mode]) == 1;
    const double amount = stereoize_gain(v[p::stereoize]);
    diffuse_.to(haas ? 0.0 : amount, steps, jump);
    haas_amount_.to(haas ? amount : 0.0, steps, jump);
    haas_frames_.to(v[p::haas_time] * 1e-3 * rate_, steps, jump);
    safe_on_ = v[p::safe] != 0.0;
    const double floor = v[p::safe_floor];
    safe_ratio_ = (1.0 - floor) / (1.0 + floor + 1e-12);
    rotation_.to(v[p::rotation] * std::numbers::pi / 180.0, steps, jump);
    balance_.to(v[p::balance], steps, jump);
    swap_.to(v[p::swap] != 0.0 ? 1.0 : 0.0, jump);
    const int invert = as_int(v[p::invert]);
    invert_left_.to((invert & 1) != 0 ? -1.0 : 1.0, jump);
    invert_right_.to((invert & 2) != 0 ? -1.0 : 1.0, jump);
    gain_.to(dsp::db_to_gain(v[p::output]), steps, jump);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    for (auto& frame : audio) {
      // Swap and polarity, ramped so a toggle never clicks.
      const double swap = swap_.next();
      double left = frame.left + (frame.right - frame.left) * swap;
      double right = frame.right + (frame.left - frame.right) * swap;
      left *= invert_left_.next();
      right *= invert_right_.next();
      meter(in_, left, right);
      const double mid = 0.5 * (left + right);
      const double side = 0.5 * (left - right);

      // Split both through the same tree; only the side is weighted.
      const double mono = mono_amount_.next();
      std::array<double, bands> mid_bands{};
      std::array<double, bands> side_bands{};
      split(mid_tree_, mid, 1.0, mid_bands);
      split(side_tree_, side, 1.0 - mono, side_bands);
      double mid_out = 0.0;
      double side_reference = 0.0;
      double side_out = 0.0;
      for (std::size_t j = 0; j < bands; ++j) {
        const double w = width_[j].next();
        mid_out += mid_bands[j];
        side_reference += side_bands[j];
        side_bands[j] *= w;
        side_out += side_bands[j];
      }

      // Stereoize: side made from the mid, above the bass.
      const double diffuse = diffuse_.next();
      const double haas = haas_amount_.next();
      const double haas_frames = haas_frames_.next();
      // Fourth order, so the bass under 300 Hz stays firmly mono.
      double source = stereoize_hp_b_.process(stereoize_hp_.process(mid_out, stereoize_low_.hp),
                                              stereoize_low_.hp);
      haas_.write(source);
      const double delayed = haas_.read(std::max(1.0, haas_frames));
      // Whole-sample delays keep every stage exactly allpass.
      for (std::size_t k = 0; k < decorrelator_.size(); ++k) {
        auto& line = decorrelator_[k];
        const double d = line.sample(decorrelator_frames_[k] - 1);
        const double w = source + decorrelator_g * d;
        line.write(w);
        source = d - decorrelator_g * w;
      }
      const double synthesised = diffuse * source + haas * delayed;
      side_out += synthesised;

      // Safe width: hold the correlation at the floor by limiting what was
      // added, never going narrower than the input's own side.
      mid_energy_ += safe_window_ * (mid_out * mid_out - mid_energy_);
      side_energy_ += safe_window_ * (side_out * side_out - side_energy_);
      reference_energy_ += safe_window_ * (side_reference * side_reference - reference_energy_);
      double target = 1.0;
      if (safe_on_) {
        const double allowed = std::max(reference_energy_, safe_ratio_ * mid_energy_);
        if (side_energy_ > allowed && side_energy_ > 1e-20) {
          target = std::sqrt(allowed / side_energy_);
        }
      }
      safe_gain_ += (target < safe_gain_ ? safe_attack_ : safe_release_) * (target - safe_gain_);
      side_out *= safe_gain_;
      for (std::size_t j = 0; j < bands; ++j) {
        auto& band = bands_[j];
        const double s = side_bands[j] * safe_gain_;
        band.mid += meter_ * (mid_bands[j] * mid_bands[j] - band.mid);
        band.side += meter_ * (s * s - band.side);
        band.cross += meter_ * (mid_bands[j] * s - band.cross);
      }

      // Back to left and right, then rotate, balance and trim.
      left = mid_out + side_out;
      right = mid_out - side_out;
      const double angle = rotation_.next();
      const double c = std::cos(angle);
      const double s = std::sin(angle);
      const double rotated_left = c * left - s * right;
      const double rotated_right = s * left + c * right;
      const double balance = balance_.next();
      const double gain = gain_.next();
      left = rotated_left * (balance > 0.0 ? 1.0 - balance : 1.0) * gain;
      right = rotated_right * (balance < 0.0 ? 1.0 + balance : 1.0) * gain;
      meter(out_, left, right);
      if (++scope_count_ == scope_decimation) {
        scope_count_ = 0;
        scope_[scope_write_] = {static_cast<float>(0.5 * (left + right)),
                                static_cast<float>(0.5 * (left - right))};
        scope_write_ = (scope_write_ + 1) % scope_points;
      }
      frame.left = static_cast<float>(left);
      frame.right = static_cast<float>(right);
    }
  }

  void reset() noexcept override {
    mid_tree_.reset();
    side_tree_.reset();
    stereoize_hp_.reset();
    stereoize_hp_b_.reset();
    for (auto& line : decorrelator_) {
      line.reset();
    }
    haas_.reset();
    mid_energy_ = 0.0;
    side_energy_ = 0.0;
    reference_energy_ = 0.0;
    safe_gain_ = 1.0;
    in_ = {};
    out_ = {};
    bands_ = {};
    scope_.fill({});
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::correlation_in] = static_cast<float>(in_.correlation());
    out[telemetry::correlation_out] = static_cast<float>(out_.correlation());
    out[telemetry::width_in_db] = static_cast<float>(in_.width_db());
    out[telemetry::width_out_db] = static_cast<float>(out_.width_db());
    out[telemetry::safe_db] = static_cast<float>(20.0 * std::log10(std::max(safe_gain_, 1e-6)));
    for (std::size_t j = 0; j < bands; ++j) {
      const auto& band = bands_[j];
      // L = M + S, R = M - S: rho = (M2 - S2) / sqrt((M2 + S2)^2 - 4 MS^2).
      const double denominator = std::sqrt(std::max(
          1e-30, (band.mid + band.side) * (band.mid + band.side) - 4.0 * band.cross * band.cross));
      out[telemetry::band_correlation + j] =
          static_cast<float>(std::clamp((band.mid - band.side) / denominator, -1.0, 1.0));
      out[telemetry::band_width_db + j] =
          static_cast<float>(10.0 * std::log10((band.side + 1e-30) / (band.mid + 1e-30)));
    }
    for (std::size_t i = 0; i < scope_points; ++i) {
      const auto& point = scope_[(scope_write_ + i) % scope_points];
      out[telemetry::scope + 2 * i] = point[0];
      out[telemetry::scope + 2 * i + 1] = point[1];
    }
    return telemetry::size;
  }

private:
  struct Meter {
    double left = 0.0;
    double right = 0.0;
    double cross = 0.0;
    [[nodiscard]] double correlation() const noexcept {
      const double d = std::sqrt(left * right);
      return d > 1e-20 ? std::clamp(cross / d, -1.0, 1.0) : 1.0;
    }
    [[nodiscard]] double width_db() const noexcept {
      // Side over mid from the L/R statistics.
      const double mid = left + right + 2.0 * cross;
      const double side = left + right - 2.0 * cross;
      return 10.0 * std::log10((side + 1e-30) / (mid + 1e-30));
    }
  };
  struct BandMeter {
    double mid = 0.0;
    double side = 0.0;
    double cross = 0.0;
  };

  void meter(Meter& m, double left, double right) const noexcept {
    m.left += meter_ * (left * left - m.left);
    m.right += meter_ * (right * right - m.right);
    m.cross += meter_ * (left * right - m.cross);
  }

  // The mono split (its low part scaled by `low_weight`), then the band
  // tree: bands 0-1 split at crossover 1 behind crossover 3's allpass, bands
  // 2-3 at crossover 3 behind crossover 1's, all below and above crossover
  // 2. Every band carries the same allpass, so they sum to it.
  void split(Tree& tree, double x, double low_weight, std::array<double, bands>& out) noexcept {
    double low = 0.0;
    double high = 0.0;
    tree.mono.process(x, mono_.lp, mono_.hp, low, high);
    const double input = high + low_weight * low;
    double below = 0.0;
    double above = 0.0;
    tree.middle.process(input, crossover_[1].lp, crossover_[1].hp, below, above);
    double a = 0.0;
    double b = 0.0;
    tree.low_comp.process(below, crossover_[2].lp, crossover_[2].hp, a, b);
    tree.low.process(a + b, crossover_[0].lp, crossover_[0].hp, out[0], out[1]);
    tree.high_comp.process(above, crossover_[0].lp, crossover_[0].hp, a, b);
    tree.high.process(a + b, crossover_[2].lp, crossover_[2].hp, out[2], out[3]);
  }

  double rate_;
  double safe_window_;
  double safe_attack_;
  double safe_release_;
  double meter_;
  std::array<Coefficients, 3> crossover_{};
  Coefficients mono_{};
  Coefficients stereoize_low_{};
  Tree mid_tree_{};
  Tree side_tree_{};
  synth::MultimodeFilter stereoize_hp_;
  synth::MultimodeFilter stereoize_hp_b_;
  std::array<dsp::Delay, decorrelator_ms.size()> decorrelator_;
  std::array<std::size_t, decorrelator_ms.size()> decorrelator_frames_{};
  dsp::Delay haas_;
  std::array<dsp::Ramp, bands> width_{};
  Slew mono_amount_;
  dsp::Ramp diffuse_;
  dsp::Ramp haas_amount_;
  dsp::Ramp haas_frames_;
  dsp::Ramp rotation_;
  dsp::Ramp balance_;
  Slew swap_;
  Slew invert_left_;
  Slew invert_right_;
  dsp::Ramp gain_;
  bool safe_on_ = false;
  double safe_ratio_ = 1.0;
  double mid_energy_ = 0.0;
  double side_energy_ = 0.0;
  double reference_energy_ = 0.0;
  double safe_gain_ = 1.0;
  Meter in_{};
  Meter out_{};
  std::array<BandMeter, bands> bands_{};
  std::array<std::array<float, 2>, scope_points> scope_{};
  std::size_t scope_write_ = 0;
  std::size_t scope_count_ = 0;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<VistaKernel>(sample_rate);
}

// The decorrelator's longest allpass rings longest: 60 dB takes
// ln(1e-3) / ln(g) passes. Crossovers and the Haas delay add a little.
std::uint64_t tail_frames(std::span<const double>, double sample_rate) {
  const double passes = std::log(1e-3) / std::log(decorrelator_g);
  const double seconds = passes * decorrelator_ms.back() * 1e-3 + max_haas_seconds + 0.1;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"vista", "Vista", 1, vista_table::parameters,
                                         vista_presets::presets};
const EffectEngine vista_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

} // namespace

const EffectEngine& engine() { return vista_engine; }

} // namespace oscillate::native_dsp::effects::vista
