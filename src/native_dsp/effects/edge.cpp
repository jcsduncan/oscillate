#include "oscillate/native_dsp/effects/edge.hpp"

#include "dsp/bs1770.hpp"
#include "edge_dsp.hpp"
#include "edge_presets.hpp"
#include "edge_table.hpp"
#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::edge {
namespace {

using edge_dsp::Curve;
using edge_table::index_of;
using native_dsp::dsp::gain_to_db;

namespace p {
constexpr std::size_t input = index_of("edge.input");
constexpr std::size_t shape = index_of("edge.shape");
constexpr std::size_t output = index_of("edge.output");
constexpr std::size_t oversampling = index_of("edge.oversampling");
constexpr std::size_t filters = index_of("edge.filters");
constexpr std::size_t link = index_of("edge.link");
constexpr std::size_t delta = index_of("edge.delta");
constexpr std::size_t compare = index_of("edge.compare");
constexpr std::size_t match = index_of("edge.match");
} // namespace p

// The ceiling is 0 dBFS; the guard holds the output this far under it so
// the float rounding of the gained output stays under it too.
constexpr double guard_margin_db = 0.01;
// Meters hold their peaks 1 s, falling 60 dB over it; the transfer dot
// follows faster.
constexpr double hold_seconds = 1.0;
constexpr double hold_fall = 1e-3;
constexpr double dot_seconds = 0.3;
// Level matching compares the output's power with the input's over about
// this long, within this range.
constexpr double match_seconds = 1.5;
constexpr double match_limit_db = 24.0;
// Below this power the input is silence and the match holds.
constexpr double match_floor_power = 1e-10;
// The clip history's pace while the transport is stopped: 120 BPM.
constexpr double stopped_beats_per_second = 2.0;
constexpr double milliseconds_per_second = 1'000.0;
constexpr double reading_floor_db = -120.0;

[[nodiscard]] double finite_or_zero(float x) noexcept {
  return std::isfinite(x) ? static_cast<double>(x) : 0.0;
}

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

class EdgeKernel final : public EffectKernel {
public:
  // Control only.
  EdgeKernel(const edge_dsp::Design& design, double sample_rate)
      : design_(design), rate_(sample_rate),
        hold_(std::exp(std::log(hold_fall) / (hold_seconds * sample_rate))),
        dot_hold_(std::exp(std::log(hold_fall) / (dot_seconds * sample_rate))),
        match_coefficient_(1.0 - std::exp(-1.0 / (match_seconds * sample_rate))),
        stopped_beats_per_frame_(stopped_beats_per_second / sample_rate) {
    for (auto& channel : channels_) {
      channel.resampler.prepare(design);
    }
    guard_.prepare(sample_rate, native_dsp::dsp::db_to_gain(-guard_margin_db));
    latency_ = design.latency + guard_.delay();
    aligned_.assign(static_cast<std::size_t>(latency_) + 1, Aligned{});
    const double step = 1.0 / (native_dsp::dsp::switch_fade_seconds * sample_rate);
    for (auto* toggle : {&link_, &delta_, &compare_, &match_}) {
      toggle->step = step;
    }
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const SynthTransport& transport) noexcept override {
    const bool jump = !primed_;
    const double steps = std::max<double>(1.0, frames);
    input_.settle(dsp::db_to_gain(v[p::input]), steps, jump);
    output_.settle(dsp::db_to_gain(v[p::output]), steps, jump);
    knee_.settle(edge_dsp::max_knee * v[p::shape], steps, jump);
    link_.to(v[p::link] != 0.0, jump);
    delta_.to(v[p::delta] != 0.0, jump);
    compare_.to(v[p::compare] != 0.0, jump);
    match_.to(v[p::match] != 0.0, jump);
    if (transport.beats_per_frame > 0.0) {
      beat_ = transport.beat_at_block_start;
      beats_per_frame_ = transport.beats_per_frame;
      if (jump) {
        beat_index_ = std::floor(beat_);
      }
    } else {
      beats_per_frame_ = stopped_beats_per_frame_;
    }
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    double block_ratio = 1.0;
    double block_guard = 1.0;
    for (auto& frame : audio) {
      const std::array<double, 2> raw{finite_or_zero(frame.left), finite_or_zero(frame.right)};
      const double input_gain = input_.next();
      const double output_gain = output_.next();
      const Curve curve = Curve::with_knee(knee_.next());
      const std::array<double, 2> driven{raw[0] * input_gain, raw[1] * input_gain};
      meter_input(driven);

      std::array<double, 2> y{};
      block_ratio = std::max(block_ratio, clip(driven, curve, y));
      block_guard = std::min(block_guard, guard_.process(y[0], y[1]));

      // The input, aligned with the output: raw for Compare, driven for Delta.
      aligned_[aligned_position_] = {raw, driven};
      aligned_position_ = (aligned_position_ + 1) % aligned_.size();
      const Aligned& dry = aligned_[aligned_position_];

      const std::array<double, 2> out{y[0] * output_gain, y[1] * output_gain};
      meter_output(out);
      match(out, dry.raw);

      const double delta = delta_.next();
      const double compare = compare_.next();
      const double bypass = 1.0 + (match_gain_ - 1.0) * match_.next();
      const double wet = (1.0 - compare) * (1.0 - delta);
      const double removed = (1.0 - compare) * delta;
      std::array<double, 2> mixed{};
      for (std::size_t c = 0; c < 2; ++c) {
        mixed[c] = wet * out[c] + removed * output_gain * (dry.driven[c] - y[c]) +
                   compare * bypass * dry.raw[c];
      }
      frame.left = static_cast<float>(mixed[0]);
      frame.right = static_cast<float>(mixed[1]);
    }
    const double frames = static_cast<double>(audio.size());
    const double clipped_db = gain_to_db(block_ratio) - gain_to_db(block_guard);
    clipped_ = std::max(clipped_ * std::pow(hold_, frames), clipped_db);
    beat_max_ = std::max(beat_max_, clipped_db);
    advance_beats(frames);
  }

  void reset() noexcept override {
    for (auto& channel : channels_) {
      channel.resampler.reset();
      channel.antialiased.reset();
      channel.out_peak.reset();
    }
    magnitude_.reset();
    guard_.reset();
    std::fill(aligned_.begin(), aligned_.end(), Aligned{});
    aligned_position_ = 0;
    input_peak_ = {};
    output_peak_ = {};
    dot_ = 0.0;
    clipped_ = 0.0;
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
    out[telemetry::dot_input] = reading(dot_);
    out[telemetry::dot_output] = reading(Curve::with_knee(knee_.value).magnitude(dot_));
    out[telemetry::latency_ms] =
        static_cast<float>(static_cast<double>(latency_) / rate_ * milliseconds_per_second);
    out[telemetry::factor] = static_cast<float>(design_.factor);
    for (std::size_t i = 0; i < history_beats; ++i) {
      out[telemetry::history + i] = history_[(history_position_ + i) % history_beats];
    }
    return telemetry::size;
  }

private:
  struct Channel {
    edge_dsp::Resampler resampler;
    edge_dsp::Antialiased antialiased;
    native_dsp::dsp::bs1770::TruePeak out_peak;
  };
  struct Aligned {
    std::array<double, 2> raw{};
    std::array<double, 2> driven{};
  };

  // Clips one frame through the oversampler into `y`; returns how far the
  // loudest top-rate sample was clipped (a ratio, 1 for none).
  double clip(const std::array<double, 2>& x, const Curve& curve,
              std::array<double, 2>& y) noexcept {
    const std::size_t factor = design_.factor;
    std::array<std::array<double, edge_dsp::max_factor>, 2> top{};
    if (design_.stages == 0) {
      top[0][0] = x[0];
      top[1][0] = x[1];
    } else {
      channels_[0].resampler.up(x[0], top[0].data());
      channels_[1].resampler.up(x[1], top[1].data());
    }
    double ratio = 1.0;
    const double linked = link_.next();
    for (std::size_t i = 0; i < factor; ++i) {
      const double l = top[0][i];
      const double r = top[1][i];
      const double loudest = std::max(std::fabs(l), std::fabs(r));
      if (loudest > curve.threshold) {
        ratio = std::max(ratio, loudest / curve.magnitude(loudest));
      }
      // Each path not heard keeps its antialiasing history, so a fade into
      // it starts clean.
      std::array<double, 2> separate{l, r};
      std::array<double, 2> together{l, r};
      if (linked < 1.0) {
        separate = clip_separate({l, r}, curve);
      } else if (design_.antialiased) {
        channels_[0].antialiased.skip(l);
        channels_[1].antialiased.skip(r);
      }
      if (linked > 0.0) {
        together = clip_linked({l, r}, loudest, curve);
      } else if (design_.antialiased) {
        magnitude_.skip(loudest);
      }
      previous_top_ = {l, r};
      top[0][i] = separate[0] + (together[0] - separate[0]) * linked;
      top[1][i] = separate[1] + (together[1] - separate[1]) * linked;
    }
    if (design_.stages == 0) {
      y = {top[0][0], top[1][0]};
    } else {
      y = {channels_[0].resampler.down(top[0].data()), channels_[1].resampler.down(top[1].data())};
    }
    return ratio;
  }

  std::array<double, 2> clip_separate(const std::array<double, 2>& x,
                                      const Curve& curve) noexcept {
    if (!design_.antialiased) {
      return {curve(x[0]), curve(x[1])};
    }
    return {channels_[0].antialiased.process(x[0], curve),
            channels_[1].antialiased.process(x[1], curve)};
  }

  // Both channels take the gain the louder one needs. With antialiasing the
  // gain is the louder one's antialiased curve over its mean, applied to
  // each channel's mean, so the linear part is the plain half-sample mean.
  std::array<double, 2> clip_linked(const std::array<double, 2>& x, double loudest,
                                    const Curve& curve) noexcept {
    if (!design_.antialiased) {
      const double gain = loudest > curve.threshold ? curve.magnitude(loudest) / loudest : 1.0;
      return {x[0] * gain, x[1] * gain};
    }
    const double previous = magnitude_.previous();
    const double shaped = magnitude_.process(loudest, curve);
    const double mean = 0.5 * (loudest + previous);
    std::array<double, 2> out{};
    for (std::size_t c = 0; c < 2; ++c) {
      const double average = 0.5 * (x[c] + previous_top_[c]);
      out[c] = mean > 0.0 ? average * (shaped / mean) : average;
    }
    return out;
  }

  void meter_input(const std::array<double, 2>& x) noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
      input_peak_[c] = std::max(input_peak_[c] * hold_, std::fabs(x[c]));
    }
    dot_ = std::max(dot_ * dot_hold_, std::max(std::fabs(x[0]), std::fabs(x[1])));
  }

  void meter_output(const std::array<double, 2>& x) noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
      const double peak = std::max(channels_[c].out_peak.push(x[c]), std::fabs(x[c]));
      output_peak_[c] = std::max(output_peak_[c] * hold_, peak);
    }
  }

  void match(const std::array<double, 2>& out, const std::array<double, 2>& raw) noexcept {
    const double out_power = 0.5 * (out[0] * out[0] + out[1] * out[1]);
    const double in_power = 0.5 * (raw[0] * raw[0] + raw[1] * raw[1]);
    out_power_ += match_coefficient_ * (out_power - out_power_);
    in_power_ += match_coefficient_ * (in_power - in_power_);
    if (in_power_ > match_floor_power) {
      const double limit = dsp::db_to_gain(match_limit_db);
      match_gain_ = std::clamp(std::sqrt(out_power_ / in_power_), 1.0 / limit, limit);
    }
  }

  // Each beat crossed files the most clipping since the last one.
  void advance_beats(double frames) noexcept {
    beat_ += beats_per_frame_ * frames;
    const double index = std::floor(beat_);
    if (index == beat_index_) {
      return;
    }
    beat_index_ = index;
    history_[history_position_] = static_cast<float>(std::max(0.0, beat_max_));
    history_position_ = (history_position_ + 1) % history_beats;
    beat_max_ = 0.0;
  }

  const edge_dsp::Design& design_;
  double rate_;
  double hold_;
  double dot_hold_;
  double match_coefficient_;
  double stopped_beats_per_frame_;
  std::uint32_t latency_ = 0;
  std::array<Channel, 2> channels_{};
  edge_dsp::Antialiased magnitude_;
  std::array<double, 2> previous_top_{};
  edge_dsp::TruePeakGuard guard_;
  std::vector<Aligned> aligned_;
  std::size_t aligned_position_ = 0;
  dsp::Ramp input_;
  dsp::Ramp output_;
  dsp::Ramp knee_;
  Toggle link_;
  Toggle delta_;
  Toggle compare_;
  Toggle match_;
  bool primed_ = false;
  std::array<double, 2> input_peak_{};
  std::array<double, 2> output_peak_{};
  double dot_ = 0.0;
  double clipped_ = 0.0;
  double out_power_ = 0.0;
  double in_power_ = 0.0;
  double match_gain_ = 1.0;
  double beat_ = 0.0;
  double beat_index_ = 0.0;
  double beats_per_frame_ = 0.0;
  double beat_max_ = 0.0;
  std::array<float, history_beats> history_{};
  std::size_t history_position_ = 0;
};

[[nodiscard]] bool valid_rate(double sample_rate) noexcept {
  return std::isfinite(sample_rate) && sample_rate > 0.0;
}

[[nodiscard]] const edge_dsp::Design& design_for(std::span<const double> values) {
  return edge_dsp::design(static_cast<std::size_t>(values[p::oversampling]),
                          static_cast<std::size_t>(values[p::filters]));
}

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return nullptr;
  }
  return std::make_unique<EdgeKernel>(design_for(values), sample_rate);
}

std::uint64_t tail_frames(std::span<const double> values, double) {
  return design_for(values).tail;
}

std::uint32_t engine_latency(std::span<const double> values, double sample_rate) {
  return latency_frames(static_cast<std::size_t>(values[p::oversampling]),
                        static_cast<std::size_t>(values[p::filters]), sample_rate);
}

const synth::EngineDescriptor descriptor{"edge", "Edge", 1, edge_table::parameters,
                                         edge_presets::presets};
const EffectEngine edge_engine{&descriptor, &create_kernel, &tail_frames, &engine_latency, false};

} // namespace

double clip(double x, double shape) noexcept {
  return Curve::with_knee(edge_dsp::max_knee * std::clamp(shape, 0.0, 1.0))(x);
}

std::uint32_t latency_frames(std::size_t oversampling, std::size_t filters,
                             double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return 0;
  }
  return edge_dsp::design(oversampling, filters).latency +
         edge_dsp::TruePeakGuard::delay_at(sample_rate);
}

const EffectEngine& engine() { return edge_engine; }

} // namespace oscillate::native_dsp::effects::edge
