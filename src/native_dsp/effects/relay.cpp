#include "oscillate/native_dsp/effects/relay.hpp"

#include "effect_dsp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "oscillate/native_dsp/transport.hpp"
#include "relay_presets.hpp"
#include "relay_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::effects::relay {
namespace {

using relay_table::Character;
using relay_table::index_of;
using relay_table::Routing;

namespace p {
constexpr std::size_t character = index_of("relay.character");
constexpr std::size_t routing = index_of("relay.routing");
constexpr std::size_t time = index_of("relay.time");
constexpr std::size_t sync = index_of("relay.sync");
constexpr std::size_t division = index_of("relay.division");
constexpr std::size_t offset = index_of("relay.offset");
constexpr std::size_t feedback = index_of("relay.feedback");
constexpr std::size_t low_cut = index_of("relay.low_cut");
constexpr std::size_t high_cut = index_of("relay.high_cut");
constexpr std::size_t drive = index_of("relay.drive");
constexpr std::size_t diffusion = index_of("relay.diffusion");
constexpr std::size_t mod_depth = index_of("relay.mod_depth");
constexpr std::size_t mod_rate = index_of("relay.mod_rate");
constexpr std::size_t ducking = index_of("relay.ducking");
constexpr std::size_t width = index_of("relay.width");
constexpr std::size_t mix = index_of("relay.mix");
constexpr std::size_t output = index_of("relay.output");
} // namespace p

constexpr std::size_t characters = 3;
constexpr std::size_t routings = 3;
// Structural changes (character, routing, diffusion engaging) ramp over
// 20 ms; a digital time change crossfades heads over 50 ms; tape and analog
// glide toward a new time with a 120 ms time constant.
constexpr double switch_seconds = native_dsp::dsp::switch_fade_seconds;
constexpr double head_fade_seconds = 0.05;
constexpr double glide_seconds = 0.12;
// Wow and flutter at full depth: +-0.3% at the rate, +-0.06% at 11.3x it.
constexpr double wow = 0.003;
constexpr double flutter = 0.0006;
constexpr double flutter_ratio = 11.3;
// The diffusion chain: four Schroeder allpasses per channel, mutually prime
// lengths, coefficient up to 0.7.
constexpr std::array<std::array<double, 4>, 2> diffuser_ms{
    {{1.31, 2.13, 3.41, 5.47}, {1.43, 2.29, 3.67, 5.21}}};
constexpr double max_diffusion = 0.7;
// Tape loses highs on every pass (a one-pole); the analog line darkens as
// its clock slows for longer delays, behind a steep (12 dB/oct) filter like
// a bucket-brigade's anti-alias and reconstruction stages.
constexpr double tape_hz = 11'000.0;
constexpr double analog_reference_seconds = 0.05;
constexpr double analog_max_hz = 12'000.0;
constexpr double analog_min_hz = 2'500.0;
// Ducking: up to 30 dB as the input rises from -50 to -10 dBFS.
constexpr double duck_range_db = 30.0;
constexpr double duck_attack_seconds = 0.005;
constexpr double duck_release_seconds = 0.25;
constexpr double meter_seconds = 0.3;
// Longest line: four seconds, a +50% right offset and modulation.
constexpr double line_seconds = max_seconds * 1.5 * (1.0 + wow + flutter) + 0.05;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }

[[nodiscard]] double log_cosh(double u) noexcept {
  const double a = std::fabs(u);
  return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
}

// First-order antiderivative antialiasing for the loop's saturators.
struct Antialiased {
  double previous = 0.0;
  template <typename Curve, typename Integral>
  [[nodiscard]] double process(double u, Curve curve, Integral integral) noexcept {
    const double du = u - previous;
    const double y = std::fabs(du) < 1e-7 ? curve(0.5 * (u + previous))
                                          : (integral(u) - integral(previous)) / du;
    previous = u;
    return y;
  }
};

struct OnePole {
  double a = 1.0;
  double y = 0.0;
  void set(double hz, double rate) noexcept { a = native_dsp::dsp::one_pole_coefficient(hz, rate); }
  [[nodiscard]] double process(double x) noexcept { return y += a * (x - y); }
};

struct Diffuser {
  dsp::Delay line;
  std::size_t frames = 1;
  [[nodiscard]] double process(double x, double g) noexcept {
    const double delayed = line.read(static_cast<double>(frames) - 1.0);
    const double v = x + g * delayed;
    line.write(v);
    return delayed - g * v;
  }
};

// A read head pair: digital crossfades from `a` to `b`; tape and analog
// glide `a` itself.
struct Head {
  double a = 0.0;
  double b = 0.0;
  double fade = 1.0; // 1 when `a` alone sounds.
};

struct Channel {
  dsp::Delay line;
  std::array<Diffuser, 4> diffusers;
  std::size_t diffuser_frames = 0;
  Head head;
  synth::MultimodeFilter low;
  synth::MultimodeFilter high;
  OnePole tape;
  synth::MultimodeFilter analog;
  synth::FilterCoefficients analog_coefficients{};
  Antialiased tape_curve;
  Antialiased analog_curve;
  double engaged = 0.0; // Diffusion chain weight.

  void reset() noexcept {
    line.reset();
    for (auto& diffuser : diffusers) {
      diffuser.line.reset();
    }
    low.reset();
    high.reset();
    tape.y = 0.0;
    analog.reset();
    tape_curve.previous = 0.0;
    analog_curve.previous = 0.0;
  }
};

class RelayKernel final : public EffectKernel {
public:
  explicit RelayKernel(double sample_rate)
      : rate_(sample_rate), switch_step_(1.0 / (switch_seconds * sample_rate)),
        head_step_(1.0 / (head_fade_seconds * sample_rate)),
        glide_(1.0 - std::exp(-1.0 / (glide_seconds * sample_rate))),
        attack_(1.0 - std::exp(-1.0 / (duck_attack_seconds * sample_rate))),
        release_(1.0 - std::exp(-1.0 / (duck_release_seconds * sample_rate))),
        meter_(1.0 - std::exp(-1.0 / (meter_seconds * sample_rate))) {
    const auto frames = static_cast<std::size_t>(std::ceil(line_seconds * sample_rate));
    for (std::size_t c = 0; c < 2; ++c) {
      auto& channel = channels_[c];
      channel.line.prepare(frames);
      channel.diffuser_frames = 0;
      for (std::size_t i = 0; i < 4; ++i) {
        auto& diffuser = channel.diffusers[i];
        diffuser.frames = std::max<std::size_t>(
            2, static_cast<std::size_t>(std::lround(diffuser_ms[c][i] * 1e-3 * sample_rate)));
        diffuser.line.prepare(diffuser.frames + 4);
        channel.diffuser_frames += diffuser.frames;
      }
      channel.tape.set(tape_hz, sample_rate);
    }
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport& transport) noexcept override {
    character_ = as_int(v[p::character]);
    routing_ = as_int(v[p::routing]);
    double seconds = v[p::time] * 1e-3;
    if (v[p::sync] != 0.0) {
      seconds = sync_seconds(static_cast<std::size_t>(v[p::division]), transport, rate_);
    }
    seconds = std::clamp(seconds, 1e-3, max_seconds);
    target_[0] = seconds * rate_;
    target_[1] = seconds * (1.0 + v[p::offset]) * rate_;
    const double steps = std::max<double>(1.0, frames);
    const bool jump = !primed_;
    feedback_.to(v[p::feedback], steps, jump);
    diffusion_.to(max_diffusion * v[p::diffusion], steps, jump);
    drive_.to(dsp::db_to_gain(v[p::drive]), steps, jump);
    depth_.to(v[p::mod_depth], steps, jump);
    ducking_.to(v[p::ducking], steps, jump);
    width_.to(v[p::width], steps, jump);
    mix_.to(v[p::mix], steps, jump);
    gain_.to(dsp::db_to_gain(v[p::output]), steps, jump);
    increment_ = v[p::mod_rate] / rate_;
    const auto low = synth::make_filter_coefficients(synth::FilterMode::highpass_12,
                                                     v[p::low_cut] / rate_, 0.0, 1.0);
    const auto high = synth::make_filter_coefficients(synth::FilterMode::lowpass_12,
                                                      v[p::high_cut] / rate_, 0.0, 1.0);
    low_ = low;
    high_ = high;
    for (std::size_t c = 0; c < 2; ++c) {
      const double line_seconds_now = target_[c] / rate_;
      const double hz =
          std::clamp(analog_max_hz * std::sqrt(analog_reference_seconds / line_seconds_now),
                     analog_min_hz, analog_max_hz);
      channels_[c].analog_coefficients =
          synth::make_filter_coefficients(synth::FilterMode::lowpass_12, hz / rate_, 0.0, 1.0);
    }
    if (jump) {
      for (std::size_t c = 0; c < 2; ++c) {
        channels_[c].head = {target_[c], target_[c], 1.0};
        channels_[c].engaged = engages(c, target_[c]) ? 1.0 : 0.0;
      }
      character_weight_.fill(0.0);
      character_weight_[static_cast<std::size_t>(character_)] = 1.0;
      routing_weight_.fill(0.0);
      routing_weight_[static_cast<std::size_t>(routing_)] = 1.0;
    }
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    for (auto& frame : audio) {
      const std::array<double, 2> x{frame.left, frame.right};
      const double mono = 0.5 * (x[0] + x[1]);
      const double feedback = feedback_.next();
      const double diffusion = diffusion_.next();
      const double drive = drive_.next();
      const double depth = depth_.next();
      const double ducking = ducking_.next();
      const double width = width_.next();
      const double mix = mix_.next();
      const double gain = gain_.next();
      step_weights();

      // Read, filter and saturate each channel's repeat.
      std::array<double, 2> repeat{};
      for (std::size_t c = 0; c < 2; ++c) {
        auto& channel = channels_[c];
        advance_head(c);
        const double phase = phase_ + (c == 1 ? 0.25 : 0.0);
        const double modulation =
            1.0 + depth * (wow * dsp::sine(phase) +
                           flutter * dsp::sine(flutter_phase_ + (c == 1 ? 0.25 : 0.0)));
        const double read = read_heads(c, modulation, diffusion);
        const double filtered = channel.high.process(channel.low.process(read, low_), high_);
        repeat[c] = character_weight_[Character::digital] * clean(filtered);
        if (character_weight_[Character::tape] > 0.0) {
          const double u = drive * channel.tape.process(filtered);
          repeat[c] += character_weight_[Character::tape] *
                       channel.tape_curve.process(
                           u, [](double s) { return std::tanh(s); }, log_cosh) /
                       drive;
        } else {
          (void)channel.tape.process(filtered);
        }
        if (character_weight_[Character::analog] > 0.0) {
          const double u = drive * channel.analog.process(filtered, channel.analog_coefficients);
          repeat[c] += character_weight_[Character::analog] *
                       channel.analog_curve.process(
                           u, [](double s) { return s / std::sqrt(1.0 + s * s); },
                           [](double s) { return std::sqrt(1.0 + s * s) - 1.0; }) /
                       drive;
        } else {
          (void)channel.analog.process(filtered, channel.analog_coefficients);
        }
      }
      phase_ = dsp::wrap(phase_ + increment_);
      flutter_phase_ = dsp::wrap(flutter_phase_ + increment_ * flutter_ratio);

      // Write back by routing, and take the wet signal the same way.
      const std::array<std::array<double, 2>, routings> writes{{
          {x[0] + feedback * repeat[0], x[1] + feedback * repeat[1]},
          {mono + feedback * repeat[1], feedback * repeat[0]},
          {mono + feedback * repeat[0], mono + feedback * repeat[1]},
      }};
      const std::array<std::array<double, 2>, routings> wets{{
          {repeat[0], repeat[1]},
          {repeat[0], repeat[1]},
          {repeat[0], repeat[0]},
      }};
      std::array<double, 2> wet{};
      for (std::size_t c = 0; c < 2; ++c) {
        double write = 0.0;
        for (std::size_t r = 0; r < routings; ++r) {
          write += routing_weight_[r] * writes[r][c];
          wet[c] += routing_weight_[r] * wets[r][c];
        }
        channels_[c].line.write(write);
      }

      // Duck under the input, set the width, mix.
      const double level = std::max(std::fabs(x[0]), std::fabs(x[1]));
      envelope_ += (level > envelope_ ? attack_ : release_) * (level - envelope_);
      const double envelope_db = 20.0 * std::log10(envelope_ + 1e-12);
      duck_db_ = -ducking * duck_range_db * std::clamp((envelope_db + 50.0) / 40.0, 0.0, 1.0);
      const double duck = dsp::db_to_gain(duck_db_);
      const double mid = 0.5 * (wet[0] + wet[1]) * duck;
      const double side = 0.5 * (wet[0] - wet[1]) * duck * width;
      const std::array<double, 2> shaped{mid + side, mid - side};
      wet_power_ += meter_ * (0.5 * (shaped[0] * shaped[0] + shaped[1] * shaped[1]) - wet_power_);
      frame.left = static_cast<float>((x[0] + (shaped[0] - x[0]) * mix) * gain);
      frame.right = static_cast<float>((x[1] + (shaped[1] - x[1]) * mix) * gain);
    }
  }

  void reset() noexcept override {
    for (auto& channel : channels_) {
      channel.reset();
    }
    phase_ = 0.0;
    flutter_phase_ = 0.0;
    envelope_ = 0.0;
    wet_power_ = 0.0;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::time_left_ms + c] = static_cast<float>(channels_[c].head.a / rate_ * 1e3);
    }
    out[telemetry::duck_db] = static_cast<float>(duck_db_);
    out[telemetry::wet_rms] = static_cast<float>(std::sqrt(wet_power_));
    return telemetry::size;
  }

private:
  // Digital is clean to 0 dBFS, then limits smoothly toward 2.
  [[nodiscard]] static double clean(double x) noexcept {
    const double a = std::fabs(x);
    return a <= 1.0 ? x : std::copysign(1.0 + std::tanh(a - 1.0), x);
  }

  // The diffusion chain can only sit inside a loop longer than itself.
  [[nodiscard]] bool engages(std::size_t c, double frames) const noexcept {
    return frames >= static_cast<double>(channels_[c].diffuser_frames) + 4.0;
  }

  // The line read `frames` after the input arrives (lines are read before
  // this frame's write).
  [[nodiscard]] double tap(std::size_t c, double frames) const noexcept {
    return channels_[c].line.read(std::max(2.0, frames) - 1.0);
  }

  // The repeat under the heads, equal-power while a digital crossfade runs,
  // through the diffusion chain when it fits: the chain's delay comes out of
  // the line's, so the loop keeps its length.
  [[nodiscard]] double read_heads(std::size_t c, double modulation, double diffusion) noexcept {
    auto& channel = channels_[c];
    const auto& head = channel.head;
    const double chain = static_cast<double>(channel.diffuser_frames);
    double plain = tap(c, head.a * modulation);
    double shortened = tap(c, head.a * modulation - chain);
    if (head.fade < 1.0) {
      const double angle = 0.5 * std::numbers::pi * head.fade;
      const double in = std::sin(angle);
      const double out = std::cos(angle);
      plain = plain * in + tap(c, head.b * modulation) * out;
      shortened = shortened * in + tap(c, head.b * modulation - chain) * out;
    }
    for (auto& diffuser : channel.diffusers) {
      shortened = diffuser.process(shortened, diffusion);
    }
    return plain + (shortened - plain) * channel.engaged;
  }

  void advance_head(std::size_t c) noexcept {
    auto& channel = channels_[c];
    auto& head = channel.head;
    const double target = target_[c];
    if (character_ == Character::digital) {
      if (head.fade < 1.0) {
        head.fade = std::min(1.0, head.fade + head_step_);
      } else if (head.a != target) {
        // Crossfade: the new head fades in from silence as the old fades out.
        head.b = head.a;
        head.a = target;
        head.fade = 0.0;
      }
    } else {
      if (head.fade < 1.0) {
        head.a = head.fade < 0.5 ? head.b : head.a;
        head.fade = 1.0;
      }
      head.a += (target - head.a) * glide_;
    }
    const double engaged = engages(c, head.a) ? 1.0 : 0.0;
    channel.engaged += std::clamp(engaged - channel.engaged, -switch_step_, switch_step_);
  }

  void step_weights() noexcept {
    for (std::size_t k = 0; k < characters; ++k) {
      const double goal = static_cast<int>(k) == character_ ? 1.0 : 0.0;
      character_weight_[k] += std::clamp(goal - character_weight_[k], -switch_step_, switch_step_);
    }
    for (std::size_t r = 0; r < routings; ++r) {
      const double goal = static_cast<int>(r) == routing_ ? 1.0 : 0.0;
      routing_weight_[r] += std::clamp(goal - routing_weight_[r], -switch_step_, switch_step_);
    }
  }

  double rate_;
  double switch_step_;
  double head_step_;
  double glide_;
  double attack_;
  double release_;
  double meter_;
  std::array<Channel, 2> channels_{};
  synth::FilterCoefficients low_{};
  synth::FilterCoefficients high_{};
  std::array<double, 2> target_{};
  std::array<double, characters> character_weight_{};
  std::array<double, routings> routing_weight_{};
  int character_ = Character::digital;
  int routing_ = Routing::stereo;
  dsp::Ramp feedback_;
  dsp::Ramp diffusion_;
  dsp::Ramp drive_;
  dsp::Ramp depth_;
  dsp::Ramp ducking_;
  dsp::Ramp width_;
  dsp::Ramp mix_;
  dsp::Ramp gain_;
  double phase_ = 0.0;
  double flutter_phase_ = 0.0;
  double increment_ = 0.0;
  double envelope_ = 0.0;
  double duck_db_ = 0.0;
  double wet_power_ = 0.0;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<RelayKernel>(sample_rate);
}

// Repeats until the feedback has fallen 60 dB, each as long as the longest
// line; synced times assume a slow 30 BPM. Feedback at or past unity can
// sustain forever, so it is held to 30 seconds.
std::uint64_t tail_frames(std::span<const double> v, double sample_rate) {
  double seconds = v[p::time] * 1e-3;
  if (v[p::sync] != 0.0) {
    const auto division = static_cast<std::size_t>(v[p::division]);
    const double beats =
        sync_in_bars(division) ? sync_lengths[division] * 4.0 : sync_lengths[division];
    seconds = beats * 2.0;
  }
  seconds = std::clamp(seconds, 1e-3, max_seconds) * (1.0 + std::max(0.0, v[p::offset]));
  const double feedback = v[p::feedback];
  double total = seconds;
  if (feedback >= 0.999) {
    total = 30.0;
  } else if (feedback > 1e-3) {
    total += seconds * std::log(1e-3) / std::log(feedback);
  }
  const double diffuser_seconds = 0.03 * (1.0 + 10.0 * v[p::diffusion]);
  return static_cast<std::uint64_t>(std::ceil((total + diffuser_seconds + 0.1) * sample_rate));
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const EngineDescriptor descriptor{"relay", "Relay", 1, relay_table::parameters,
                                  relay_presets::presets};
const EffectEngine relay_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

} // namespace

const EffectEngine& engine() { return relay_engine; }

} // namespace oscillate::native_dsp::effects::relay
