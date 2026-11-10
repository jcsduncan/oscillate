#include "oscillate/native_dsp/effects/strike.hpp"

#include "dsp/crossover.hpp"
#include "edge_dsp.hpp"
#include "edge_table.hpp"
#include "effect_dsp.hpp"
#include "linear_bank.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "strike_presets.hpp"
#include "strike_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::strike {
namespace {

namespace base = native_dsp::dsp;
namespace t = strike_table;
using base::gain_to_db;
using strike_table::index_of;

namespace p {
constexpr std::size_t bands = index_of("strike.bands");
constexpr std::size_t xover_1 = index_of("strike.xover_1");
constexpr std::size_t attack_time = index_of("strike.attack_time");
constexpr std::size_t sustain_time = index_of("strike.sustain_time");
constexpr std::size_t phase = index_of("strike.phase");
constexpr std::size_t lookahead = index_of("strike.lookahead");
constexpr std::size_t link = index_of("strike.link");
constexpr std::size_t mix = index_of("strike.mix");
constexpr std::size_t output = index_of("strike.output");
constexpr std::size_t clip = index_of("strike.clip");
constexpr std::size_t ceiling = index_of("strike.ceiling");
constexpr std::size_t first_band = t::first_band;
} // namespace p

static_assert(t::band_count == max_bands);
static_assert(t::crossover_defaults.size() == max_crossovers);
static_assert(max_bands <= dsp::LinearBank::max_bands);
static_assert(telemetry::size <= max_telemetry_values);

constexpr double milliseconds_per_second = 1'000.0;
constexpr double decibels_per_bel = 10.0;

// The quadrature pair: two chains of second-order allpasses in z^-2
// (a - z^-2) / (1 - a z^-2), the first delayed by one sample, whose outputs
// stay 90 degrees apart within 0.7 degrees from 20 Hz to 23.5 kHz at
// 48 kHz (O. Niemitalo's design: these are the squares of its
// coefficients). Their squares summed are the analytic signal's power, which
// a steady sine holds steady.
constexpr std::size_t quadrature_sections = 4;
constexpr std::array<double, quadrature_sections> quadrature_roots_a{
    0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
constexpr std::array<double, quadrature_sections> quadrature_roots_b{
    0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};

constexpr std::array<double, quadrature_sections>
squares(const std::array<double, quadrature_sections>& roots) {
  std::array<double, quadrature_sections> out{};
  for (std::size_t i = 0; i < quadrature_sections; ++i) {
    out[i] = roots[i] * roots[i];
  }
  return out;
}
constexpr auto quadrature_a = squares(quadrature_roots_a);
constexpr auto quadrature_b = squares(quadrature_roots_b);

// Added to every power before a ratio is taken (about -240 dBFS), so
// silence reads as no change; and the level under which a follower is
// taken as silent (it never decays into denormals).
constexpr double power_floor = 1e-24;
constexpr double silent_power = 1e-30;
// The ratios at dead_db and full_db, as powers.
const double dead_ratio = std::pow(10.0, dead_db / decibels_per_bel);
const double full_ratio = std::pow(10.0, full_db / decibels_per_bel);
// A gain this close to its target lands on it, so a band at rest is
// exactly unity.
constexpr double settle_db = 1e-9;

// Crossovers stay a third of an octave apart and under the Nyquist guard.
const double crossover_spacing = std::cbrt(2.0);

// Clip: Edge's curve with half its widest knee, four times oversampled
// through its lighter filters.
constexpr double clip_shape = 0.5;
constexpr std::size_t clip_oversampling = edge_table::x4;
constexpr std::size_t clip_filters = edge_table::light;

// After the input stops the crossovers ring this long.
constexpr double tail_seconds = 0.05;

// Meters hold their peaks 1 s, falling 60 dB over it.
constexpr double hold_seconds = 1.0;
constexpr double hold_fall = 1e-3;
constexpr double reading_floor_db = -120.0;

[[nodiscard]] double finite_or_zero(float x) noexcept {
  return std::isfinite(x) ? static_cast<double>(x) : 0.0;
}

[[nodiscard]] std::size_t choice(double value, std::size_t count) noexcept {
  return std::min(static_cast<std::size_t>(std::max(0.0, value)), count - 1);
}

[[nodiscard]] bool valid_rate(double sample_rate) noexcept {
  return std::isfinite(sample_rate) && sample_rate > 0.0;
}

// The one-pole coefficient whose step response reaches 1 - 1/e in `seconds`.
[[nodiscard]] double coefficient(double seconds, double rate) noexcept {
  return 1.0 - std::exp(-1.0 / (seconds * rate));
}

// 0 at or under dead_db, 1 at or over full_db, linear in decibels between.
[[nodiscard]] double weight(double ratio) noexcept {
  if (ratio <= dead_ratio) {
    return 0.0;
  }
  if (ratio >= full_ratio) {
    return 1.0;
  }
  return (decibels_per_bel * std::log10(ratio) - dead_db) / (full_db - dead_db);
}

// One second-order allpass in z^-2.
struct Section {
  double x1 = 0.0;
  double x2 = 0.0;
  double y1 = 0.0;
  double y2 = 0.0;
  [[nodiscard]] double process(double x, double a) noexcept {
    const double y = a * (x + y2) - x2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
};

// One channel's quadrature pair.
struct Quadrature {
  std::array<Section, quadrature_sections> a{};
  std::array<Section, quadrature_sections> b{};
  double delayed = 0.0;
  [[nodiscard]] double power(double x) noexcept {
    double first = x;
    double second = x;
    for (std::size_t i = 0; i < quadrature_sections; ++i) {
      first = a[i].process(first, quadrature_a[i]);
      second = b[i].process(second, quadrature_b[i]);
    }
    const double real = delayed;
    delayed = first;
    return real * real + second * second;
  }
};

// One band's detector on one channel.
struct Follower {
  Quadrature quadrature{};
  double level = 0.0; // smoothed analytic power
  double rise = 0.0;  // follows the level up with Attack Time
  double fall = 0.0;  // follows it down with Sustain Time
  double gain_db = 0.0;
  double out_db = 0.0;
  double out_gain = 1.0;
};

[[nodiscard]] double flushed(double x) noexcept { return x < silent_power ? 0.0 : x; }

// A 0..1 switch that glides over dsp::switch_fade_seconds.
struct Toggle {
  double value = 0.0;
  double target = 0.0;
  double step = 1.0;
  void to(bool on, bool jump) noexcept {
    target = on ? 1.0 : 0.0;
    if (jump) {
      value = target;
    }
  }
  double next() noexcept {
    value += std::clamp(target - value, -step, step);
    return value;
  }
};

struct Layout {
  std::size_t bands = 1;
  std::array<double, max_crossovers> crossovers{};
};

Layout layout_for(std::span<const double> v, double rate) noexcept {
  Layout layout;
  layout.bands = choice(v[p::bands], t::band_counts.size()) + 1;
  const double top = std::min(t::crossover_max, base::nyquist_guard * rate);
  const std::size_t count = layout.bands - 1;
  for (std::size_t k = 0; k < count; ++k) {
    const double room = std::pow(crossover_spacing, static_cast<double>(count - 1 - k));
    const double floor = k == 0 ? t::crossover_min : layout.crossovers[k - 1] * crossover_spacing;
    layout.crossovers[k] = std::clamp(v[p::xover_1 + k], floor, std::max(floor, top / room));
  }
  return layout;
}

// What fixes the latency.
struct Setup {
  std::uint32_t lookahead = 0; // frames
  bool linear = false;
  bool clip = false;
};

class StrikeKernel final : public EffectKernel {
public:
  // Control only.
  StrikeKernel(const Setup& setup, double sample_rate)
      : rate_(sample_rate), lookahead_(setup.lookahead), clip_(setup.clip),
        design_(edge_dsp::design(clip_oversampling, clip_filters)),
        level_coefficient_(coefficient(level_seconds, sample_rate)),
        rise_release_(coefficient(rise_release_seconds, sample_rate)),
        gain_coefficient_(coefficient(gain_seconds, sample_rate)),
        hold_(std::exp(std::log(hold_fall) / (hold_seconds * sample_rate))),
        slice_frames_(std::max<std::size_t>(
            1, static_cast<std::size_t>(std::lround(history_seconds * sample_rate /
                                                    static_cast<double>(history_slices))))) {
    delayed_.assign(static_cast<std::size_t>(lookahead_) + 1, Frame{});
    if (setup.linear) {
      bank_ = std::make_unique<dsp::LinearBank>(dsp::bank_frames(sample_rate), sample_rate);
      bank_latency_ = dsp::bank_frames(sample_rate).latency();
    }
    if (clip_) {
      for (auto& resampler : resamplers_) {
        resampler.prepare(design_);
      }
    }
    const double step = 1.0 / (base::switch_fade_seconds * sample_rate);
    for (auto& solo : heard_) {
      solo.step = step;
    }
    reset();
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
    if (bank_) {
      bank_->set(layout_.bands, layout_.crossovers);
    }
    attack_coefficient_ = coefficient(v[p::attack_time] / milliseconds_per_second, rate_);
    sustain_fall_ = std::exp(-1.0 / (v[p::sustain_time] / milliseconds_per_second * rate_));
    bool any_solo = false;
    for (std::size_t b = 0; b < layout_.bands; ++b) {
      any_solo = any_solo || v[p::first_band + b * t::band::size + t::band::solo] != 0.0;
    }
    for (std::size_t b = 0; b < max_bands; ++b) {
      const std::size_t first = p::first_band + b * t::band::size;
      attack_db_[b] = v[first + t::band::attack];
      sustain_db_[b] = v[first + t::band::sustain];
      const bool solo = v[first + t::band::solo] != 0.0;
      heard_[b].to(b < layout_.bands && (!any_solo || solo), jump);
    }
    link_ = std::clamp(v[p::link], 0.0, 1.0);
    mix_.settle(std::clamp(v[p::mix], 0.0, 1.0), steps, jump);
    output_.settle(base::db_to_gain(v[p::output]), steps, jump);
    ceiling_.settle(base::db_to_gain(v[p::ceiling]), steps, jump);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    const std::size_t n = layout_.bands;
    const std::size_t size = delayed_.size();
    block_gain_.fill(0.0);
    double block_clip = 1.0;
    for (auto& frame : audio) {
      const std::array<double, 2> in{finite_or_zero(frame.left), finite_or_zero(frame.right)};
      for (std::size_t c = 0; c < 2; ++c) {
        input_peak_[c] = std::max(input_peak_[c] * hold_, std::fabs(in[c]));
      }
      std::array<std::array<double, max_bands>, 2> split{};
      if (bank_) {
        bank_->process(in[0], in[1], split[0], split[1]);
      } else {
        trees_[0].process(in[0], split[0]);
        trees_[1].process(in[1], split[1]);
      }

      // The bands now feed the detectors; the bands lookahead frames ago
      // take the gains.
      Frame& slot = delayed_[position_];
      slot = {split[0], split[1]};
      position_ = (position_ + 1) % size;
      const Frame& heard = delayed_[position_];

      const double mix = mix_.next();
      const double output = output_.next();
      std::array<double, 2> dry{};
      std::array<double, 2> wet{};
      for (std::size_t b = 0; b < n; ++b) {
        const std::array<double, 2> gain = detect(b, split[0][b], split[1][b]);
        const double solo = heard_[b].next();
        for (std::size_t c = 0; c < 2; ++c) {
          dry[c] += heard[c][b];
          wet[c] += heard[c][b] * (1.0 + mix * (gain[c] - 1.0)) * solo;
        }
      }
      for (std::size_t b = n; b < max_bands; ++b) {
        (void)heard_[b].next();
      }
      wet[0] *= output;
      wet[1] *= output;
      file(dry, wet);

      std::array<double, 2> out = wet;
      if (clip_) {
        block_clip = std::max(block_clip, clip(out));
      }
      for (std::size_t c = 0; c < 2; ++c) {
        output_peak_[c] = std::max(output_peak_[c] * hold_, std::fabs(out[c]));
      }
      frame.left = static_cast<float>(out[0]);
      frame.right = static_cast<float>(out[1]);
    }
    const double frames = static_cast<double>(audio.size());
    clipped_ = std::max(clipped_ * std::pow(hold_, frames), gain_to_db(block_clip));
  }

  void reset() noexcept override {
    for (auto& tree : trees_) {
      tree.reset();
    }
    if (bank_) {
      bank_->reset();
    }
    for (auto& band : followers_) {
      for (auto& follower : band) {
        follower = Follower{};
      }
    }
    std::ranges::fill(delayed_, Frame{});
    position_ = 0;
    for (auto& resampler : resamplers_) {
      resampler.reset();
    }
    input_peak_ = {};
    output_peak_ = {};
    clipped_ = 0.0;
    block_gain_.fill(0.0);
    slice_input_ = 0.0;
    slice_output_ = 0.0;
    slice_gain_.fill(0.0);
    slice_count_ = 0;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    const auto reading = [](double gain) {
      return static_cast<float>(gain_to_db(gain, reading_floor_db));
    };
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::input_peak + c] = reading(input_peak_[c]);
      out[telemetry::output_peak + c] = reading(output_peak_[c]);
    }
    out[telemetry::clipped] = static_cast<float>(clipped_);
    out[telemetry::latency_ms] =
        static_cast<float>(static_cast<double>(latency()) / rate_ * milliseconds_per_second);
    out[telemetry::band_count] = static_cast<float>(layout_.bands);
    for (std::size_t k = 0; k < max_crossovers; ++k) {
      out[telemetry::crossovers + k] =
          k + 1 < layout_.bands ? static_cast<float>(layout_.crossovers[k]) : 0.0F;
    }
    for (std::size_t b = 0; b < max_bands; ++b) {
      out[telemetry::band_gain + b] = b < layout_.bands ? static_cast<float>(block_gain_[b]) : 0.0F;
    }
    for (std::size_t i = 0; i < history_slices; ++i) {
      const std::size_t at = (history_position_ + i) % history_slices;
      out[telemetry::history_input + i] = history_input_[at];
      out[telemetry::history_output + i] = history_output_[at];
      for (std::size_t b = 0; b < max_bands; ++b) {
        out[telemetry::history_gain + b * history_slices + i] = history_gain_[b][at];
      }
    }
    return telemetry::size;
  }

private:
  using Frame = std::array<std::array<double, max_bands>, 2>;

  [[nodiscard]] std::uint32_t latency() const noexcept {
    return bank_latency_ + lookahead_ + (clip_ ? design_.latency : 0);
  }

  // Band b's detectors take one frame; returns each channel's gain (linear).
  std::array<double, 2> detect(std::size_t b, double left, double right) noexcept {
    auto& pair = followers_[b];
    const std::array<double, 2> x{left, right};
    for (std::size_t c = 0; c < 2; ++c) {
      auto& f = pair[c];
      f.level = flushed(f.level + level_coefficient_ * (f.quadrature.power(x[c]) - f.level));
    }
    const double loudest = std::max(pair[0].level, pair[1].level);
    const double attack_db = attack_db_[b];
    const double sustain_db = sustain_db_[b];
    std::array<double, 2> gain{};
    for (std::size_t c = 0; c < 2; ++c) {
      auto& f = pair[c];
      const double level = f.level + link_ * (loudest - f.level);
      const double towards = level > f.rise ? attack_coefficient_ : rise_release_;
      f.rise = flushed(f.rise + towards * (level - f.rise));
      f.fall = flushed(std::max(f.fall * sustain_fall_, level));
      const double rising = (level + power_floor) / (f.rise + power_floor);
      const double fallen = (f.fall + power_floor) / (level + power_floor);
      const double target = attack_db * weight(rising) + sustain_db * weight(fallen);
      f.gain_db += gain_coefficient_ * (target - f.gain_db);
      if (std::fabs(target - f.gain_db) < settle_db) {
        f.gain_db = target;
      }
      if (f.gain_db != f.out_db) {
        f.out_db = f.gain_db;
        f.out_gain = base::db_to_gain(f.gain_db);
      }
      gain[c] = f.out_gain;
      if (std::fabs(f.gain_db) > std::fabs(block_gain_[b])) {
        block_gain_[b] = f.gain_db;
      }
      if (std::fabs(f.gain_db) > std::fabs(slice_gain_[b])) {
        slice_gain_[b] = f.gain_db;
      }
    }
    return gain;
  }

  // The sum through Edge's curve under the ceiling, both channels taking
  // the gain the louder needs; returns how far the loudest top-rate sample
  // came down (a ratio, 1 for none).
  double clip(std::array<double, 2>& y) noexcept {
    const double ceiling = ceiling_.next();
    const auto curve = edge_dsp::Curve::with_knee(edge_dsp::max_knee * clip_shape);
    std::array<std::array<double, edge_dsp::max_factor>, 2> top{};
    resamplers_[0].up(y[0] / ceiling, top[0].data());
    resamplers_[1].up(y[1] / ceiling, top[1].data());
    double ratio = 1.0;
    for (std::size_t i = 0; i < design_.factor; ++i) {
      const double loudest = std::max(std::fabs(top[0][i]), std::fabs(top[1][i]));
      if (loudest > curve.threshold) {
        const double gain = curve.magnitude(loudest) / loudest;
        ratio = std::max(ratio, 1.0 / gain);
        top[0][i] *= gain;
        top[1][i] *= gain;
      }
    }
    y = {resamplers_[0].down(top[0].data()) * ceiling,
         resamplers_[1].down(top[1].data()) * ceiling};
    return ratio;
  }

  // The view: the dry and shaped sums' peaks and each band's gain.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at the one call.
  void file(const std::array<double, 2>& dry, const std::array<double, 2>& wet) noexcept {
    slice_input_ = std::max(slice_input_, std::max(std::fabs(dry[0]), std::fabs(dry[1])));
    slice_output_ = std::max(slice_output_, std::max(std::fabs(wet[0]), std::fabs(wet[1])));
    if (++slice_count_ < slice_frames_) {
      return;
    }
    history_input_[history_position_] = static_cast<float>(slice_input_);
    history_output_[history_position_] = static_cast<float>(slice_output_);
    for (std::size_t b = 0; b < max_bands; ++b) {
      history_gain_[b][history_position_] =
          b < layout_.bands ? static_cast<float>(slice_gain_[b]) : 0.0F;
    }
    history_position_ = (history_position_ + 1) % history_slices;
    slice_input_ = 0.0;
    slice_output_ = 0.0;
    slice_gain_.fill(0.0);
    slice_count_ = 0;
  }

  double rate_;
  std::uint32_t lookahead_;
  bool clip_;
  const edge_dsp::Design& design_;
  double level_coefficient_;
  double rise_release_;
  double gain_coefficient_;
  double hold_;
  std::size_t slice_frames_;
  Layout layout_{};
  std::array<base::CrossoverTree<max_crossovers>, 2> trees_{};
  std::unique_ptr<dsp::LinearBank> bank_;
  std::uint32_t bank_latency_ = 0;
  std::array<std::array<Follower, 2>, max_bands> followers_{};
  std::vector<Frame> delayed_;
  std::size_t position_ = 0;
  std::array<edge_dsp::Resampler, 2> resamplers_{};
  std::array<Toggle, max_bands> heard_{};
  std::array<double, max_bands> attack_db_{};
  std::array<double, max_bands> sustain_db_{};
  double attack_coefficient_ = 1.0;
  double sustain_fall_ = 0.0;
  double link_ = 1.0;
  base::Ramp mix_{};
  base::Ramp output_{};
  base::Ramp ceiling_{};
  bool primed_ = false;
  std::array<double, 2> input_peak_{};
  std::array<double, 2> output_peak_{};
  double clipped_ = 0.0;
  std::array<double, max_bands> block_gain_{};
  double slice_input_ = 0.0;
  double slice_output_ = 0.0;
  std::array<double, max_bands> slice_gain_{};
  std::size_t slice_count_ = 0;
  std::array<float, history_slices> history_input_{};
  std::array<float, history_slices> history_output_{};
  std::array<std::array<float, history_slices>, max_bands> history_gain_{};
  std::size_t history_position_ = 0;
};

[[nodiscard]] std::size_t lookahead_choice(std::span<const double> values) noexcept {
  return choice(values[p::lookahead], t::lookahead_ms.size());
}

[[nodiscard]] bool clip_on(std::span<const double> values) noexcept {
  return values[p::clip] != 0.0;
}

[[nodiscard]] bool linear_phase(std::span<const double> values) noexcept {
  return choice(values[p::phase], t::phases.size()) == t::Phase::linear;
}

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return nullptr;
  }
  const Setup setup{latency_frames(lookahead_choice(values), false, false, sample_rate),
                    linear_phase(values), clip_on(values)};
  return std::make_unique<StrikeKernel>(setup, sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return 0;
  }
  const std::uint64_t clip_tail =
      clip_on(values) ? edge_dsp::design(clip_oversampling, clip_filters).tail : 0;
  const std::uint64_t bank_tail =
      linear_phase(values) ? dsp::bank_frames(sample_rate).half_length : 0;
  return static_cast<std::uint64_t>(std::ceil(tail_seconds * sample_rate)) + clip_tail + bank_tail;
}

std::uint32_t engine_latency(std::span<const double> values, double sample_rate) {
  return latency_frames(lookahead_choice(values), linear_phase(values), clip_on(values),
                        sample_rate);
}

const EngineDescriptor descriptor{"strike", "Strike", 1, t::parameters, strike_presets::presets};
const EffectEngine strike_engine{&descriptor, &create_kernel, &tail_frames, &engine_latency};

} // namespace

std::uint32_t latency_frames(std::size_t lookahead, bool linear, bool clip, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return 0;
  }
  const std::uint32_t bank = linear ? dsp::bank_frames(sample_rate).latency() : 0;
  const auto& table = t::lookahead_ms;
  const double ms = table[std::min(lookahead, table.size() - 1)];
  const auto ahead =
      static_cast<std::uint32_t>(std::lround(ms / milliseconds_per_second * sample_rate));
  return bank + ahead + (clip ? edge_dsp::design(clip_oversampling, clip_filters).latency : 0);
}

const EffectEngine& engine() { return strike_engine; }

} // namespace oscillate::native_dsp::effects::strike
