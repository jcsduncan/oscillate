#include "oscillate/native_dsp/effects/orbit.hpp"

#include "effect_dsp.hpp"
#include "orbit_presets.hpp"
#include "orbit_table.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/crossfade.hpp"
#include "oscillate/native_dsp/transport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::effects::orbit {
namespace {

using orbit_table::index_of;
using orbit_table::Mode;

namespace p {
constexpr std::size_t mode = index_of("orbit.mode");
constexpr std::size_t rate = index_of("orbit.rate");
constexpr std::size_t sync = index_of("orbit.sync");
constexpr std::size_t division = index_of("orbit.division");
constexpr std::size_t shape = index_of("orbit.shape");
constexpr std::size_t depth = index_of("orbit.depth");
constexpr std::size_t stereo = index_of("orbit.stereo");
constexpr std::size_t time = index_of("orbit.time");
constexpr std::size_t feedback = index_of("orbit.feedback");
constexpr std::size_t voices = index_of("orbit.voices");
constexpr std::size_t stages = index_of("orbit.stages");
constexpr std::size_t centre = index_of("orbit.centre");
constexpr std::size_t tone = index_of("orbit.tone");
constexpr std::size_t mix = index_of("orbit.mix");
constexpr std::size_t output = index_of("orbit.output");
} // namespace p

// Chorus and vibrato sweep their delay +-30% of Time at full depth; the
// flanger sweeps from Time down to 5% of it; the dimension +-25%.
constexpr double chorus_swing = 0.3;
constexpr double flanger_swing = 0.95;
constexpr double dimension_swing = 0.25;
// Voice delays spread around Time so voices never line up.
constexpr std::array<double, 4> voice_spread{0.0, -0.17, 0.11, -0.07};
// The string ensemble: three taps 120 degrees apart, a slow sweep and a
// fast shimmer at nine times its rate.
constexpr double ensemble_ms = 5.0;
constexpr double ensemble_slow_ms = 1.5;
constexpr double ensemble_fast_ms = 0.3;
constexpr double ensemble_fast_ratio = 9.0;
// The phaser sweeps +-3 octaves around its centre at full depth.
constexpr double phaser_octaves = 3.0;
constexpr std::size_t max_stages = 12;
constexpr double max_delay_seconds = 0.1;
constexpr double tone_hz = 2'500.0;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }
[[nodiscard]] bool uses_feedback_line(int mode) noexcept {
  return mode == Mode::chorus || mode == Mode::flanger;
}

struct Allpass {
  double x1 = 0.0;
  double y1 = 0.0;
  [[nodiscard]] double process(double x, double a) noexcept {
    const double y = a * x + x1 - a * y1;
    x1 = x;
    y1 = y;
    return y;
  }
};

class OrbitKernel final : public EffectKernel {
public:
  explicit OrbitKernel(double sample_rate)
      : rate_(sample_rate), tone_k_(std::tan(std::numbers::pi * tone_hz / sample_rate)) {
    const auto frames = static_cast<std::size_t>(std::ceil(max_delay_seconds * sample_rate));
    for (std::size_t c = 0; c < 2; ++c) {
      input_[c].prepare(frames);
      feedback_[c].prepare(frames);
    }
    fade_.prepare(sample_rate);
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const Transport& transport) noexcept override {
    const int mode = as_int(v[p::mode]);
    if (!primed_) {
      mode_ = mode;
    } else if (mode != mode_ && !fade_.active()) {
      fading_mode_ = mode_;
      mode_ = mode;
      enter(mode_);
      fade_.start();
    }
    shape_ = as_int(v[p::shape]);
    voices_ = as_int(v[p::voices]) + 1;
    stages_ = orbit_table::stage_values[static_cast<std::size_t>(as_int(v[p::stages]))];
    if (v[p::sync] != 0.0 && transport.beats_per_frame > 0.0) {
      const auto division = static_cast<std::size_t>(v[p::division]);
      phase_ = dsp::wrap(sync_cycles(division, transport));
      increment_ = transport.beats_per_frame / sync_beats(division, transport);
    } else {
      increment_ = v[p::rate] / rate_;
    }
    const double steps = std::max<double>(1.0, frames);
    const bool jump = !primed_;
    depth_.to(v[p::depth], steps, jump);
    stereo_.to(v[p::stereo] / 360.0, steps, jump);
    time_.to(v[p::time] * 1e-3 * rate_, steps, jump);
    feedback_amount_.to(v[p::feedback], steps, jump);
    centre_.to(v[p::centre], steps, jump);
    mix_.to(v[p::mix], steps, jump);
    gain_.to(dsp::db_to_gain(v[p::output]), steps, jump);
    // A high shelf darkens the wet path like a bucket-brigade line; at
    // 0 dB it is exactly the identity, so dry and wet stay aligned.
    const double darken = dsp::db_to_gain(v[p::tone]);
    for (auto& shelf : tone_filter_) {
      shelf.set(darken, 1.0, 1.0, 1.0, tone_k_);
    }
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    for (auto& frame : audio) {
      Frame f;
      f.phase = phase_;
      f.fast = fast_phase_;
      f.depth = depth_.next();
      f.stereo = stereo_.next();
      f.time = time_.next();
      f.feedback = feedback_amount_.next();
      f.centre = centre_.next();
      f.x = {frame.left, frame.right};
      phase_ = dsp::wrap(phase_ + increment_);
      fast_phase_ = dsp::wrap(fast_phase_ + increment_ * ensemble_fast_ratio);
      const double mix = mix_.next();
      const double gain = gain_.next();
      const double weight = fade_.active() ? fade_.next() : 0.0;

      std::array<double, 2> loop{};
      auto wet = render(mode_, f, loop, true);
      if (weight > 0.0) {
        std::array<double, 2> unused{};
        const auto old = render(fading_mode_, f, unused, false);
        for (std::size_t c = 0; c < 2; ++c) {
          wet[c] += (old[c] - wet[c]) * weight;
        }
      }
      // Keep every line fed so whichever mode comes next has history.
      const bool looped = uses_feedback_line(mode_);
      for (std::size_t c = 0; c < 2; ++c) {
        input_[c].write(f.x[c]);
        feedback_[c].write(f.x[c] + (looped ? f.feedback * loop[c] : 0.0));
      }
      std::array<double, 2> out{};
      for (std::size_t c = 0; c < 2; ++c) {
        const double toned = tone_filter_[c].process(wet[c]);
        out[c] = (f.x[c] + (toned - f.x[c]) * mix) * gain;
      }
      frame.left = static_cast<float>(out[0]);
      frame.right = static_cast<float>(out[1]);
    }
  }

  void reset() noexcept override {
    for (std::size_t c = 0; c < 2; ++c) {
      input_[c].reset();
      feedback_[c].reset();
      tone_filter_[c].reset();
    }
    reset_phaser();
    phase_ = 0.0;
    fast_phase_ = 0.0;
    fade_.stop();
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      out[telemetry::lfo_left + c] = static_cast<float>(lfo_[c]);
      out[telemetry::sweep_left + c] = static_cast<float>(sweep_[c]);
    }
    return telemetry::size;
  }

private:
  struct Frame {
    double phase = 0.0;
    double fast = 0.0;
    double depth = 0.0;
    double stereo = 0.0;
    double time = 0.0; // Frames.
    double feedback = 0.0;
    double centre = 0.0;
    std::array<double, 2> x{};
  };

  [[nodiscard]] double shape(double phase) const noexcept {
    return shape_ == 1 ? dsp::triangle(phase) : dsp::sine(phase);
  }
  // Unipolar, starting at 0: the flanger's and tremolo's sweep.
  [[nodiscard]] double rising(double phase) const noexcept {
    return 0.5 + 0.5 * shape(phase - 0.25);
  }
  // Lines are read before this frame's write: `delay` frames after the
  // input that arrives now.
  [[nodiscard]] static double tap(const dsp::Delay& line, double delay) noexcept {
    return line.read(std::max(2.0, delay) - 1.0);
  }
  [[nodiscard]] double mono_tap(double delay) const noexcept {
    return 0.5 * (tap(input_[0], delay) + tap(input_[1], delay));
  }

  void enter(int mode) noexcept {
    if (mode == Mode::phaser) {
      reset_phaser();
    }
  }
  void reset_phaser() noexcept {
    for (auto& chain : allpasses_) {
      chain.fill({});
    }
    phaser_last_ = {};
  }

  // One mode's wet output. `loop` receives what feeds the feedback line;
  // `live` modes also update telemetry.
  std::array<double, 2> render(int mode, const Frame& f, std::array<double, 2>& loop,
                               bool live) noexcept {
    std::array<double, 2> wet{};
    std::array<double, 2> lfo{};
    std::array<double, 2> sweep{};
    const double ms = 1e3 / rate_;
    switch (mode) {
    case Mode::ensemble: {
      std::array<double, 3> taps{};
      for (std::size_t i = 0; i < 3; ++i) {
        const double third = static_cast<double>(i) / 3.0;
        const double delay_ms =
            ensemble_ms + f.depth * (ensemble_slow_ms * dsp::sine(f.phase + third) +
                                     ensemble_fast_ms * dsp::sine(f.fast + third));
        taps[i] = mono_tap(delay_ms / ms);
        if (i == 0) {
          sweep = {delay_ms, delay_ms};
        }
      }
      wet = {(taps[0] + 0.5 * taps[1]) / 1.5, (taps[2] + 0.5 * taps[1]) / 1.5};
      lfo = {dsp::sine(f.phase), dsp::sine(f.phase + 2.0 / 3.0)};
      break;
    }
    case Mode::dimension: {
      const double b = shape(f.phase);
      const double a_delay = f.time * (1.0 + dimension_swing * f.depth * b);
      const double b_delay = f.time * (1.0 - dimension_swing * f.depth * b);
      wet = {mono_tap(a_delay), mono_tap(b_delay)};
      lfo = {b, -b};
      sweep = {a_delay * ms, b_delay * ms};
      break;
    }
    case Mode::phaser: {
      for (std::size_t c = 0; c < 2; ++c) {
        lfo[c] = shape(f.phase + (c == 1 ? f.stereo : 0.0));
        const double hz = std::clamp(f.centre * std::exp2(phaser_octaves * f.depth * lfo[c]), 20.0,
                                     native_dsp::dsp::nyquist_guard * rate_);
        const double k = std::tan(std::numbers::pi * hz / rate_);
        const double a = (k - 1.0) / (k + 1.0);
        double y = f.x[c] + f.feedback * phaser_last_[c];
        for (int s = 0; s < stages_; ++s) {
          y = allpasses_[c][static_cast<std::size_t>(s)].process(y, a);
        }
        phaser_last_[c] = y;
        wet[c] = y;
        sweep[c] = hz;
      }
      break;
    }
    case Mode::tremolo: {
      for (std::size_t c = 0; c < 2; ++c) {
        const double phase = f.phase + (c == 1 ? f.stereo : 0.0);
        lfo[c] = shape(phase);
        const double g = 1.0 - f.depth * rising(phase);
        wet[c] = f.x[c] * g;
        sweep[c] = g;
      }
      break;
    }
    case Mode::flanger: {
      for (std::size_t c = 0; c < 2; ++c) {
        const double phase = f.phase + (c == 1 ? f.stereo : 0.0);
        lfo[c] = shape(phase);
        const double delay = f.time * (1.0 - flanger_swing * f.depth * rising(phase));
        wet[c] = tap(feedback_[c], delay);
        loop[c] = wet[c];
        sweep[c] = delay * ms;
      }
      break;
    }
    default: { // Chorus and vibrato.
      const bool vibrato = mode == Mode::vibrato;
      const int voices = vibrato ? 1 : voices_;
      for (std::size_t c = 0; c < 2; ++c) {
        const double phase = f.phase + (c == 1 ? f.stereo : 0.0);
        double sum = 0.0;
        for (int v = 0; v < voices; ++v) {
          const auto voice = static_cast<std::size_t>(v);
          const double offset = static_cast<double>(v) / static_cast<double>(voices);
          const double b = shape(phase + offset);
          const double delay =
              f.time * (1.0 + voice_spread[voice]) * (1.0 + chorus_swing * f.depth * b);
          sum += tap(vibrato ? input_[c] : feedback_[c], delay);
          if (v == 0) {
            lfo[c] = b;
            sweep[c] = delay * ms;
          }
        }
        wet[c] = sum / static_cast<double>(voices);
        loop[c] = wet[c];
      }
      break;
    }
    }
    if (live) {
      lfo_ = lfo;
      sweep_ = sweep;
    }
    return wet;
  }

  double rate_;
  std::array<dsp::Delay, 2> input_;
  std::array<dsp::Delay, 2> feedback_;
  std::array<std::array<Allpass, max_stages>, 2> allpasses_{};
  std::array<double, 2> phaser_last_{};
  std::array<dsp::Shelf, 2> tone_filter_{};
  double tone_k_;
  synth::SwitchFade fade_;
  int mode_ = Mode::chorus;
  int fading_mode_ = Mode::chorus;
  int shape_ = 0;
  int voices_ = 2;
  int stages_ = 4;
  double phase_ = 0.0;
  double fast_phase_ = 0.0;
  double increment_ = 0.0;
  dsp::Ramp depth_;
  dsp::Ramp stereo_;
  dsp::Ramp time_;
  dsp::Ramp feedback_amount_;
  dsp::Ramp centre_;
  dsp::Ramp mix_;
  dsp::Ramp gain_;
  std::array<double, 2> lfo_{};
  std::array<double, 2> sweep_{};
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<OrbitKernel>(sample_rate);
}

// Feedback rings for the repeats it takes to fall 60 dB, each as long as
// the longest delay (or the phaser chain's group delay at its lowest
// sweep); every mode then clears its longest line.
std::uint64_t tail_frames(std::span<const double> v, double sample_rate) {
  const int mode = as_int(v[p::mode]);
  const double feedback = std::fabs(v[p::feedback]);
  const double longest = v[p::time] * 1e-3 * (1.0 + chorus_swing) * 1.2;
  double seconds = std::max(longest, (ensemble_ms + ensemble_slow_ms + ensemble_fast_ms) * 1e-3);
  if ((uses_feedback_line(mode) || mode == Mode::phaser) && feedback > 1e-3) {
    const double repeats = std::log(1e-3) / std::log(feedback);
    double period = longest;
    if (mode == Mode::phaser) {
      const double low = std::max(20.0, v[p::centre] * std::exp2(-phaser_octaves * v[p::depth]));
      const auto stages = orbit_table::stage_values[static_cast<std::size_t>(v[p::stages])];
      period = stages / (std::numbers::pi * low);
    }
    seconds += repeats * period;
  }
  return static_cast<std::uint64_t>(std::ceil((seconds + 0.05) * sample_rate));
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"orbit", "Orbit", 1, orbit_table::parameters,
                                         orbit_presets::presets};
const EffectEngine orbit_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

} // namespace

const EffectEngine& engine() { return orbit_engine; }

} // namespace oscillate::native_dsp::effects::orbit
