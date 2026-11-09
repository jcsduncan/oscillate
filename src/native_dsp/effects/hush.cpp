#include "oscillate/native_dsp/effects/hush.hpp"

#include "dsp/biquad.hpp"
#include "effect_dsp.hpp"
#include "hush_presets.hpp"
#include "hush_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::hush {
namespace {

using hush_table::index_of;
using native_dsp::dsp::Biquad;
using native_dsp::dsp::BiquadCoefficients;
using native_dsp::dsp::gain_to_db;

namespace p {
constexpr std::size_t threshold = index_of("hush.threshold");
constexpr std::size_t hysteresis = index_of("hush.hysteresis");
constexpr std::size_t range = index_of("hush.range");
constexpr std::size_t mode = index_of("hush.mode");
constexpr std::size_t ratio = index_of("hush.ratio");
constexpr std::size_t attack = index_of("hush.attack");
constexpr std::size_t hold = index_of("hush.hold");
constexpr std::size_t release = index_of("hush.release");
constexpr std::size_t lookahead = index_of("hush.lookahead");
constexpr std::size_t key = index_of("hush.key");
constexpr std::size_t key_hp = index_of("hush.key_hp");
constexpr std::size_t key_lp = index_of("hush.key_lp");
constexpr std::size_t listen = index_of("hush.listen");
constexpr std::size_t link = index_of("hush.link");
constexpr std::size_t flip = index_of("hush.flip");
} // namespace p

constexpr double milliseconds_per_second = 1'000.0;
// Below this the detector reads silence (it never decays into denormals).
constexpr double detector_floor = 1e-30;
// With a muting range the gain moves between unity and this, where it
// mutes exactly.
constexpr double mute_floor_db = -120.0;
// Attack and release cross at least this many decibels, so a range of 0 dB
// still lets a gain left below it climb back.
constexpr double min_span_db = 1.0;
// Meters hold their peaks 1 s, falling 60 dB over it; the reduction meter
// falls faster, so it follows the gate.
constexpr double hold_seconds = 1.0;
constexpr double reduction_seconds = 0.3;
constexpr double hold_fall = 1e-3;
constexpr double reading_floor_db = -120.0;
// The key filter: two fourth-order Butterworth filters (24 dB/octave).
constexpr std::size_t key_order = 4;
constexpr std::size_t key_sections = key_order / 2;

[[nodiscard]] double finite_or_zero(float x) noexcept {
  return std::isfinite(x) ? static_cast<double>(x) : 0.0;
}

[[nodiscard]] std::size_t choice(double value, std::size_t count) noexcept {
  return std::min(static_cast<std::size_t>(std::max(0.0, value)), count - 1);
}

// The Q of section `k` of an order-`key_order` Butterworth filter.
[[nodiscard]] double butterworth_section_q(std::size_t k) noexcept {
  const double angle =
      std::numbers::pi * static_cast<double>(2 * k + 1) / static_cast<double>(2 * key_order);
  return 1.0 / (2.0 * std::cos(angle));
}

// The RBJ second-order high-pass or low-pass at `hz`, held below the
// Nyquist guard.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): named at every call.
[[nodiscard]] BiquadCoefficients pass(bool high, double hz, double rate, double q) noexcept {
  const double w =
      2.0 * std::numbers::pi * std::min(hz, native_dsp::dsp::nyquist_guard * rate) / rate;
  const double c = std::cos(w);
  const double alpha = std::sin(w) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double edge = high ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
  const double middle = high ? -(1.0 + c) : 1.0 - c;
  return {edge / a0, middle / a0, edge / a0, -2.0 * c / a0, (1.0 - alpha) / a0};
}

[[nodiscard]] std::uint32_t frames_for(double milliseconds, double rate) noexcept {
  return static_cast<std::uint32_t>(std::lround(milliseconds / milliseconds_per_second * rate));
}

[[nodiscard]] bool valid_rate(double sample_rate) noexcept {
  return std::isfinite(sample_rate) && sample_rate > 0.0;
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

class HushKernel final : public EffectKernel {
public:
  // Control only.
  HushKernel(std::uint32_t lookahead, double sample_rate)
      : rate_(sample_rate), lookahead_(lookahead),
        detector_fall_(std::exp(-1.0 / (detector_fall_seconds * sample_rate))),
        hold_(std::exp(std::log(hold_fall) / (hold_seconds * sample_rate))),
        reduction_hold_(std::exp(std::log(hold_fall) / (reduction_seconds * sample_rate))),
        slice_frames_(std::max<std::size_t>(
            1, static_cast<std::size_t>(std::lround(history_seconds * sample_rate /
                                                    static_cast<double>(history_slices))))) {
    for (auto& line : delay_) {
      line.assign(static_cast<std::size_t>(lookahead_) + 1, 0.0);
    }
    for (auto& line : key_delay_) {
      line.assign(static_cast<std::size_t>(lookahead_) + 1, 0.0);
    }
    listen_.step = 1.0 / (native_dsp::dsp::switch_fade_seconds * sample_rate);
  }

  void control(std::span<const double> v, std::uint32_t, const Transport&) noexcept override {
    const bool jump = !primed_;
    open_db_ = v[p::threshold];
    close_db_ = v[p::threshold] - v[p::hysteresis];
    mute_ = v[p::range] <= hush_table::mute_range_db;
    floor_db_ = mute_ ? mute_floor_db : v[p::range];
    expander_ = choice(v[p::mode], hush_table::modes.size()) == hush_table::expander;
    ratio_ = v[p::ratio];
    const double span = std::max(-floor_db_, min_span_db);
    attack_step_ = span / std::max(1.0, v[p::attack] / milliseconds_per_second * rate_);
    release_step_ = span / std::max(1.0, v[p::release] / milliseconds_per_second * rate_);
    hold_frames_ = frames_for(v[p::hold], rate_);
    use_sidechain_ = choice(v[p::key], hush_table::keys.size()) == hush_table::sidechain;
    link_ = v[p::link];
    flip_ = v[p::flip] != 0.0;
    listen_.to(v[p::listen] != 0.0, jump);
    if (v[p::key_hp] != key_hp_ || v[p::key_lp] != key_lp_) {
      key_hp_ = v[p::key_hp];
      key_lp_ = v[p::key_lp];
      for (std::size_t s = 0; s < key_sections; ++s) {
        const double q = butterworth_section_q(s);
        const auto high = pass(true, key_hp_, rate_, q);
        const auto low = pass(false, key_lp_, rate_, q);
        for (auto& channel : channels_) {
          channel.high[s].set(high);
          channel.low[s].set(low);
        }
      }
    }
    if (jump) {
      start_closed();
    }
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    const bool keyed = use_sidechain_ && !sidechain.empty();
    const std::size_t size = delay_[0].size();
    for (std::size_t i = 0; i < audio.size(); ++i) {
      const std::array<double, 2> in{finite_or_zero(audio[i].left), finite_or_zero(audio[i].right)};
      const std::array<double, 2> raw_key =
          keyed ? std::array<double, 2>{finite_or_zero(sidechain[i].left),
                                        finite_or_zero(sidechain[i].right)}
                : in;
      std::array<double, 2> key{};
      for (std::size_t c = 0; c < 2; ++c) {
        key[c] = filter(channels_[c], raw_key[c]);
        auto& envelope = channels_[c].envelope;
        envelope = std::max(std::fabs(key[c]), envelope * detector_fall_);
        if (envelope < detector_floor) {
          envelope = 0.0;
        }
      }
      const double loudest = std::max(channels_[0].envelope, channels_[1].envelope);
      std::array<double, 2> gain{};
      for (std::size_t c = 0; c < 2; ++c) {
        auto& channel = channels_[c];
        const double level = channel.envelope + link_ * (loudest - channel.envelope);
        gain[c] = gain_of(channel, step(channel, gain_to_db(level)));
      }

      // The audio and the key, delayed by the lookahead.
      std::array<double, 2> dry{};
      std::array<double, 2> heard{};
      for (std::size_t c = 0; c < 2; ++c) {
        delay_[c][position_] = in[c];
        key_delay_[c][position_] = key[c];
      }
      position_ = (position_ + 1) % size;
      for (std::size_t c = 0; c < 2; ++c) {
        dry[c] = delay_[c][position_];
        heard[c] = key_delay_[c][position_];
      }

      const double listen = listen_.next();
      std::array<double, 2> out{};
      for (std::size_t c = 0; c < 2; ++c) {
        out[c] = (1.0 - listen) * dry[c] * gain[c] + listen * heard[c];
        input_peak_[c] = std::max(input_peak_[c] * hold_, std::fabs(in[c]));
        output_peak_[c] = std::max(output_peak_[c] * hold_, std::fabs(out[c]));
      }
      audio[i].left = static_cast<float>(out[0]);
      audio[i].right = static_cast<float>(out[1]);

      const double least = std::min(gain[0], gain[1]);
      const double cut = least > 0.0 ? -gain_to_db(least) : reduction_floor_db;
      reduction_ = std::max(reduction_ * reduction_hold_, std::min(cut, reduction_floor_db));
      slice_peak_ = std::max(slice_peak_, std::max(std::fabs(dry[0]), std::fabs(dry[1])));
      slice_gain_ = std::min(slice_gain_, least);
      if (++slice_count_ == slice_frames_) {
        file_slice();
      }
    }
  }

  void reset() noexcept override {
    for (auto& channel : channels_) {
      for (auto& section : channel.high) {
        section.reset();
      }
      for (auto& section : channel.low) {
        section.reset();
      }
    }
    for (auto& line : delay_) {
      std::ranges::fill(line, 0.0);
    }
    for (auto& line : key_delay_) {
      std::ranges::fill(line, 0.0);
    }
    position_ = 0;
    start_closed();
    input_peak_ = {};
    output_peak_ = {};
    reduction_ = 0.0;
    slice_peak_ = 0.0;
    slice_gain_ = 1.0;
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
    out[telemetry::key_level] = reading(std::max(channels_[0].envelope, channels_[1].envelope));
    out[telemetry::reduction] = static_cast<float>(reduction_);
    const double open = (channels_[0].open ? 1.0 : 0.0) + (channels_[1].open ? 1.0 : 0.0);
    out[telemetry::open] = static_cast<float>(open / 2.0);
    out[telemetry::open_threshold] = static_cast<float>(open_db_);
    out[telemetry::close_threshold] = static_cast<float>(close_db_);
    out[telemetry::latency_ms] =
        static_cast<float>(static_cast<double>(lookahead_) / rate_ * milliseconds_per_second);
    for (std::size_t i = 0; i < history_slices; ++i) {
      const std::size_t at = (history_position_ + i) % history_slices;
      out[telemetry::history_peak + i] = history_peak_[at];
      out[telemetry::history_gain + i] = history_gain_[at];
    }
    return telemetry::size;
  }

private:
  struct Channel {
    std::array<Biquad, key_sections> high{};
    std::array<Biquad, key_sections> low{};
    double envelope = 0.0;
    bool open = false;
    std::uint32_t held = 0;
    // The gain before flip, in dB: 0 open, floor_db_ closed.
    double gain_db = 0.0;
    // The last gain_of, so a steady gain costs no pow.
    double out_db = 0.0;
    double out_gain = 1.0;
  };

  [[nodiscard]] static double filter(Channel& channel, double x) noexcept {
    for (auto& section : channel.high) {
      x = section.process(x);
    }
    for (auto& section : channel.low) {
      x = section.process(x);
    }
    return x;
  }

  // One frame of the gain computer: the state with its hysteresis and hold,
  // the static target, and the constant-rate move towards it.
  double step(Channel& channel, double level_db) noexcept {
    if (level_db >= open_db_) {
      channel.open = true;
      channel.held = hold_frames_;
    } else if (channel.open) {
      if (level_db >= close_db_) {
        channel.held = hold_frames_;
      } else if (channel.held > 0) {
        --channel.held;
      } else {
        channel.open = false;
      }
    }
    double target = 0.0;
    if (!channel.open) {
      target = expander_ ? std::clamp((level_db - open_db_) * (ratio_ - 1.0), floor_db_, 0.0)
                         : floor_db_;
    }
    const double distance = target - channel.gain_db;
    const double limit = distance > 0.0 ? attack_step_ : release_step_;
    channel.gain_db =
        std::fabs(distance) <= limit ? target : channel.gain_db + (distance > 0.0 ? limit : -limit);
    return flip_ ? floor_db_ - channel.gain_db : channel.gain_db;
  }

  [[nodiscard]] double gain_of(Channel& channel, double db) const noexcept {
    if (mute_ && db <= mute_floor_db) {
      return 0.0;
    }
    if (db != channel.out_db) {
      channel.out_db = db;
      channel.out_gain = native_dsp::dsp::db_to_gain(db);
    }
    return channel.out_gain;
  }

  void start_closed() noexcept {
    for (auto& channel : channels_) {
      channel.envelope = 0.0;
      channel.open = false;
      channel.held = 0;
      channel.gain_db = floor_db_;
    }
  }

  void file_slice() noexcept {
    history_peak_[history_position_] = static_cast<float>(slice_peak_);
    history_gain_[history_position_] = static_cast<float>(slice_gain_);
    history_position_ = (history_position_ + 1) % history_slices;
    slice_peak_ = 0.0;
    slice_gain_ = 1.0;
    slice_count_ = 0;
  }

  double rate_;
  std::uint32_t lookahead_;
  double detector_fall_;
  double hold_;
  double reduction_hold_;
  std::size_t slice_frames_;
  std::array<Channel, 2> channels_{};
  std::array<std::vector<double>, 2> delay_{};
  std::array<std::vector<double>, 2> key_delay_{};
  std::size_t position_ = 0;
  Toggle listen_;
  double open_db_ = 0.0;
  double close_db_ = 0.0;
  double floor_db_ = mute_floor_db;
  bool mute_ = true;
  bool expander_ = false;
  double ratio_ = 1.0;
  double attack_step_ = 0.0;
  double release_step_ = 0.0;
  std::uint32_t hold_frames_ = 0;
  bool use_sidechain_ = true;
  double link_ = 1.0;
  bool flip_ = false;
  double key_hp_ = 0.0;
  double key_lp_ = 0.0;
  bool primed_ = false;
  std::array<double, 2> input_peak_{};
  std::array<double, 2> output_peak_{};
  double reduction_ = 0.0;
  double slice_peak_ = 0.0;
  double slice_gain_ = 1.0;
  std::size_t slice_count_ = 0;
  std::array<float, history_slices> history_peak_{};
  std::array<float, history_slices> history_gain_{};
  std::size_t history_position_ = 0;
};

[[nodiscard]] std::size_t lookahead_choice(std::span<const double> values) noexcept {
  return choice(values[p::lookahead], hush_table::lookahead_ms.size());
}

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return nullptr;
  }
  return std::make_unique<HushKernel>(latency_frames(lookahead_choice(values), sample_rate),
                                      sample_rate);
}

// The output is the input times a gain: it stops with the input.
std::uint64_t tail_frames(std::span<const double>, double) { return 0; }

std::uint32_t engine_latency(std::span<const double> values, double sample_rate) {
  return latency_frames(lookahead_choice(values), sample_rate);
}

const EngineDescriptor descriptor{"hush", "Hush", 1, hush_table::parameters, hush_presets::presets};
const EffectEngine hush_engine{&descriptor, &create_kernel, &tail_frames, &engine_latency, true};

} // namespace

std::uint32_t latency_frames(std::size_t lookahead, double sample_rate) {
  if (!valid_rate(sample_rate)) {
    return 0;
  }
  const auto& table = hush_table::lookahead_ms;
  return frames_for(table[std::min(lookahead, table.size() - 1)], sample_rate);
}

const EffectEngine& engine() { return hush_engine; }

} // namespace oscillate::native_dsp::effects::hush
