#include "oscillate/native_dsp/effects/chop.hpp"

#include "chop_dsp.hpp"
#include "chop_presets.hpp"
#include "chop_table.hpp"
#include "dsp/ramp.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace oscillate::native_dsp::effects {
namespace chop {

namespace {

using chop_table::index_of;

namespace p {
constexpr std::size_t mode = index_of("chop.mode");
constexpr std::size_t steps = index_of("chop.steps");
constexpr std::size_t division = index_of("chop.division");
constexpr std::size_t smooth = index_of("chop.smooth");
constexpr std::size_t shape = index_of("chop.shape");
constexpr std::size_t swing = index_of("chop.swing");
constexpr std::size_t rotate = index_of("chop.rotate");
constexpr std::size_t repeat_pitch = index_of("chop.repeat_pitch");
constexpr std::size_t repeat_decay = index_of("chop.repeat_decay");
constexpr std::size_t depth = index_of("chop.depth");
constexpr std::size_t mix = index_of("chop.mix");
constexpr std::size_t seed = index_of("chop.seed");
constexpr std::size_t first_step = chop_table::first_step;
} // namespace p

constexpr double seconds_per_minute = 60.0;
constexpr double seconds_per_millisecond = 1e-3;
constexpr double semitones_per_octave = 12.0;
// A step edge this close after a frame (in steps) belongs to that frame, so
// the transport's tick rounding never moves an edge by one. A frame is
// more than 1e-4 steps at any tempo a project allows.
constexpr double edge_epsilon_steps = 1e-6;
// The song position moving further than this from where the last tick
// predicted, in beats, is a seek or a loop: the pattern restarts there.
// Tick rounding and a tempo ramp across one block stay far below it.
constexpr double jump_tolerance_beats = 1e-3;
// Swing at 100 % starts each even step half a step late: the first of each
// pair takes 75 % of it.
constexpr double max_swing_delay = 0.5;
// Shape at Soft lets a gate go over this many times Smooth.
constexpr double release_stretch = 12.0;
// The widest repeat slice is half its step (repeat 1: twice).
constexpr double widest_slice = 0.5;
// A repeat's crossfade is at most half its slice, so each repeat is heard.
constexpr double widest_crossfade = 0.5;
// The interpolator reads two frames either side of a position.
constexpr std::size_t interpolation_guard = 4;
constexpr std::int64_t no_step = std::numeric_limits<std::int64_t>::min();

static_assert(chop_table::division_beats[0] >= chop_table::division_beats[1] &&
                  chop_table::division_beats[1] >= chop_table::division_beats[2],
              "the first division is the longest step");

[[nodiscard]] std::size_t choice(double value) noexcept {
  return static_cast<std::size_t>(std::max(0.0, value));
}

[[nodiscard]] std::int64_t modulo(std::int64_t a, std::int64_t n) noexcept {
  return ((a % n) + n) % n;
}

[[nodiscard]] std::int64_t floor_div(std::int64_t a, std::int64_t n) noexcept {
  return (a - modulo(a, n)) / n;
}

// The longest step of a division a repeat may capture: swung to the full,
// at the lowest tempo repeats support.
[[nodiscard]] double longest_step_seconds(double division_beats) noexcept {
  return division_beats * (1.0 + max_swing_delay) * seconds_per_minute / min_repeat_bpm;
}

// Each capture buffer holds the widest slice of the longest step and its
// crossfade.
[[nodiscard]] std::size_t capture_frames(double sample_rate) {
  const double seconds = widest_slice * longest_step_seconds(chop_table::division_beats[0]) +
                         chop_table::max_smooth_ms * seconds_per_millisecond;
  return static_cast<std::size_t>(std::ceil(seconds * sample_rate)) + interpolation_guard;
}

struct StepValues {
  double level = 1.0;
  double length = 1.0;
  std::size_t ratchet = 1;
  std::size_t repeat = 0;
  double chance = 1.0;
};

struct Sample {
  double left = 0.0;
  double right = 0.0;
};

class ChopKernel final : public EffectKernel {
public:
  ChopKernel(std::span<const double> values, double sample_rate) : rate_(sample_rate) {
    const auto frames = capture_frames(sample_rate);
    for (auto& buffer : buffers_)
      buffer.prepare(frames);
    read_values(values);
    capture_.reset();
  }

  void control(std::span<const double> v, std::uint32_t frames_to_next_tick,
               const Transport& transport) noexcept override {
    const auto steps = static_cast<double>(frames_to_next_tick);
    read_values(v);
    depth_.to(v[p::depth], steps, !primed_);
    mix_.to(v[p::mix], steps, !primed_);
    // Switches fade over Smooth, re-aimed every tick over at least the tick.
    const auto fade = [&](native_dsp::dsp::Ramp& ramp, double target) {
      ramp.settle(target, std::max(steps, smooth_frames_ * std::fabs(target - ramp.value)),
                  !primed_);
    };
    fade(gate_amount_, mode_ == chop_table::repeat ? 0.0 : 1.0);
    // With no tempo there is no grid: Chop fades to the input.
    fade(engaged_, transport.beats_per_frame > 0.0 ? 1.0 : 0.0);
    // The position is anchored at the tick's frame, which locate() brings.
    pending_ = transport;
    pending_valid_ = true;
    primed_ = true;
  }

  // The project frame of the next frame. After a tick, the song position
  // is anchored here: every later frame's beat is the anchor's plus whole
  // frames at the tempo, so any block split renders the same positions.
  // A changed tempo re-anchors; a jump (seek, loop) restarts the pattern.
  void locate(std::uint64_t project_frame) noexcept override {
    // A frame other than the next is a jump (a loop): start the step afresh.
    if (static_cast<std::int64_t>(project_frame) != frame_)
      forget_position();
    frame_ = static_cast<std::int64_t>(project_frame);
    if (!pending_valid_)
      return;
    pending_valid_ = false;
    const double bpf = pending_.beats_per_frame;
    if (!(bpf > 0.0)) {
      tempo_known_ = false;
      anchored_ = false;
      forget_position();
      return;
    }
    const double beat = pending_.beat_at_block_start;
    bool jumped = !anchored_;
    if (anchored_) {
      const double predicted = anchor_beat_ + static_cast<double>(frame_ - anchor_frame_) * bpf_;
      jumped = std::fabs(beat - predicted) > jump_tolerance_beats;
    }
    if (jumped)
      forget_position();
    if (jumped || bpf != bpf_) {
      anchor_frame_ = frame_;
      anchor_beat_ = beat;
      bpf_ = bpf;
      anchored_ = true;
    }
    tempo_known_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    (void)sidechain;
    for (auto& frame : audio) {
      const engine::StereoFrame in = frame;
      Sample y{in.left, in.right};
      std::size_t point = 0;
      if (tempo_known_) {
        const double beat = anchor_beat_ + static_cast<double>(frame_ - anchor_frame_) * bpf_;
        const Where where = locate_step(beat);
        if (where.step != step_)
          enter_step(where);
        gate(where);
        y = stutter(where, in);
        // The playhead runs straight through the pattern (swing bends the
        // steps, not the time).
        const double count = static_cast<double>(steps_);
        const double in_pattern = beat / step_beats_ / count;
        phase_ = in_pattern - std::floor(in_pattern);
        point = static_cast<std::size_t>(phase_ * static_cast<double>(view_points));
      }
      const double env = env_.value();
      env_.advance();
      const double gain = 1.0 - gate_amount_.next() * depth_.next() * (1.0 - env);
      const double wet = mix_.next() * engaged_.next();
      const double left = in.left + wet * (y.left * gain - in.left);
      const double right = in.right + wet * (y.right * gain - in.right);
      frame = {static_cast<float>(left), static_cast<float>(right)};
      if (tempo_known_) {
        capture_.add(point, std::max(std::fabs(in.left), std::fabs(in.right)),
                     std::max(std::fabs(left), std::fabs(right)), gain);
      }
      ++frame_;
    }
  }

  void reset() noexcept override {
    anchored_ = false;
    tempo_known_ = false;
    forget_position();
    // The gain carries on from where it was into the new place's first edge.
    env_.hold(env_.value());
    capture_.reset();
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size)
      return 0;
    const bool live = tempo_known_ && step_ != no_step;
    out[telemetry::phase] = live ? static_cast<float>(phase_) : -1.0F;
    out[telemetry::step] = live ? static_cast<float>(index_) : -1.0F;
    out[telemetry::slot] = live ? static_cast<float>(slot_) : -1.0F;
    out[telemetry::pattern_beats] = static_cast<float>(static_cast<double>(steps_) * step_beats_);
    out[telemetry::pattern] = static_cast<float>(pattern_);
    for (std::size_t i = 0; i < max_steps; ++i) {
      const bool plays = i < steps_ && chance_roll(seed_, pattern_, i) < values_[i].chance;
      out[telemetry::played + i] = plays ? 1.0F : 0.0F;
    }
    std::ranges::copy(capture_.input(), out.begin() + telemetry::input);
    std::ranges::copy(capture_.output(), out.begin() + telemetry::output);
    std::ranges::copy(capture_.gain(), out.begin() + telemetry::gain);
    return telemetry::size;
  }

private:
  // Where a frame falls: the step's count from the song start (its slot,
  // swing applied), the frames since it started and its length in frames.
  struct Where {
    std::int64_t step = 0;
    double into = 0.0;
    double length = 0.0;
    double epsilon = 0.0; // edge_epsilon_steps in frames.
  };

  // A repeat step's replay: its capture buffer, slices, crossfade, and each
  // slot's playback rate and level (slot 0 is the input, live).
  struct Plan {
    bool active = false;
    std::size_t buffer = 0;
    std::size_t count = 0;
    double slice = 0.0;
    double cross = 0.0;
    double start = 0.0; // Frames into the step of the first captured frame.
    std::array<double, chop_table::max_repeats_per_step> rate{};
    std::array<double, chop_table::max_repeats_per_step> level{};
  };

  // The last repeat of the step before, still sounding as it crossfades
  // into this one.
  struct Tail {
    bool active = false;
    std::size_t buffer = 0;
    double rate = 1.0;
    double level = 1.0;
    double slice = 0.0;
    double cross = 0.0;
    double start = 0.0;
    double elapsed = 0.0;
  };

  void read_values(std::span<const double> v) noexcept {
    mode_ = static_cast<int>(std::min(choice(v[p::mode]), chop_table::modes.size() - 1));
    steps_ = chop_table::step_count_values[std::min(choice(v[p::steps]),
                                                    chop_table::step_count_values.size() - 1)];
    step_beats_ = chop_table::division_beats[std::min(choice(v[p::division]),
                                                      chop_table::division_beats.size() - 1)];
    smooth_frames_ = v[p::smooth] * seconds_per_millisecond * rate_;
    shape_ = std::clamp(v[p::shape], 0.0, 1.0);
    swing_delay_ = std::clamp(v[p::swing], 0.0, 1.0) * max_swing_delay;
    rotate_ = choice(v[p::rotate]);
    pitch_ = v[p::repeat_pitch];
    decay_ = std::clamp(v[p::repeat_decay], 0.0, 1.0);
    seed_ = static_cast<std::uint32_t>(choice(v[p::seed]));
    for (std::size_t s = 0; s < max_steps; ++s) {
      const std::size_t base = p::first_step + s * chop_table::step_fields;
      auto& step = values_[s];
      step.level = std::clamp(v[base + chop_table::level], 0.0, 1.0);
      step.length = std::clamp(v[base + chop_table::length], 0.0, 1.0);
      step.ratchet = std::clamp<std::size_t>(choice(v[base + chop_table::ratchet]), 1,
                                             chop_table::max_ratchet);
      step.repeat = std::min(choice(v[base + chop_table::repeat_count]), chop_table::max_repeat);
      step.chance = v[base + chop_table::chance];
    }
  }

  void forget_position() noexcept {
    step_ = no_step;
    hit_ = -1;
    closed_ = true;
    plan_.active = false;
    tail_.active = false;
    fresh_ = true;
  }

  [[nodiscard]] double release_frames() const noexcept {
    return smooth_frames_ * (1.0 + shape_ * release_stretch);
  }

  // Steps from the beat: each pair's first step takes 1 + swing of the
  // pair's two, the second the rest.
  [[nodiscard]] Where locate_step(double beat) const noexcept {
    const double steps = beat / step_beats_;
    const double frames = step_beats_ / bpf_;
    const double pair = std::floor((steps + edge_epsilon_steps) / 2.0);
    const double place = std::max(0.0, steps - 2.0 * pair);
    const double first = 1.0 + swing_delay_;
    const auto base = 2 * static_cast<std::int64_t>(pair);
    const double epsilon = edge_epsilon_steps * frames;
    if (place + edge_epsilon_steps < first)
      return {base, place * frames, first * frames, epsilon};
    return {base + 1, std::max(0.0, place - first) * frames, (1.0 - swing_delay_) * frames,
            epsilon};
  }

  // A new step: which pattern step plays in this slot, whether its chance
  // lets it, and whether it repeats. A repeat that was playing crossfades
  // out into it.
  void enter_step(const Where& where) noexcept {
    tail_.active = plan_.active;
    if (plan_.active) {
      tail_.buffer = plan_.buffer;
      tail_.rate = plan_.rate[plan_.count - 1];
      tail_.level = plan_.level[plan_.count - 1];
      tail_.slice = plan_.slice;
      tail_.cross = plan_.cross;
      tail_.start = plan_.start;
      tail_.elapsed = where.into;
    }
    step_ = where.step;
    hit_ = -1;
    const auto count = static_cast<std::int64_t>(steps_);
    slot_ = modulo(where.step, count);
    pattern_ = floor_div(where.step, count);
    index_ = static_cast<std::size_t>(modulo(slot_ - static_cast<std::int64_t>(rotate_), count));
    const auto& step = values_[index_];
    played_ = chance_roll(seed_, pattern_, index_) < step.chance;

    // A repeat captures from the step's first frame, so a step entered
    // part way (after a seek) plays through.
    plan_.active = false;
    if (mode_ == chop_table::gate || step.repeat == 0 || !played_ || where.into >= 1.0)
      return;
    const std::size_t repeats = std::size_t{1} << step.repeat;
    const double slice = where.length / static_cast<double>(repeats);
    const double cross = std::min(smooth_frames_, widest_crossfade * slice);
    const std::size_t buffer = 1 - plan_.buffer;
    if (slice + cross + static_cast<double>(interpolation_guard) >
        static_cast<double>(buffers_[buffer].capacity()))
      return;
    plan_.active = true;
    plan_.buffer = buffer;
    plan_.count = repeats;
    plan_.slice = slice;
    plan_.cross = cross;
    plan_.start = where.into;
    buffers_[buffer].restart();
    const double last = static_cast<double>(repeats - 1);
    for (std::size_t q = 0; q < repeats; ++q) {
      const double along = static_cast<double>(q) / last;
      plan_.rate[q] = std::exp2(-pitch_ * along / semitones_per_octave);
      plan_.level[q] = 1.0 - decay_ * along;
    }
  }

  // The gate: each hit (the step, or each ratchet) opens to the step's
  // level over Smooth from its start, and closes after its length over the
  // release. Edges start where they fall between frames, to the sample.
  void gate(const Where& where) noexcept {
    const double late = fresh_ ? 0.0 : 1.0; // After a reset, fade from the gain now.
    fresh_ = false;
    const auto& step = values_[index_];
    const double sub = where.length / static_cast<double>(step.ratchet);
    const auto hit =
        std::min(static_cast<int>(std::max(0.0, std::floor((where.into + where.epsilon) / sub))),
                 static_cast<int>(step.ratchet) - 1);
    const double into = std::max(0.0, where.into - static_cast<double>(hit) * sub);
    if (hit != hit_) {
      hit_ = hit;
      const bool open = played_ && step.length > 0.0;
      // A gate stays open at least as long as its edge, so it reaches its
      // level before it lets go.
      open_frames_ =
          open ? std::clamp(std::max(step.length * sub, std::min(smooth_frames_, sub)), 0.0, sub)
               : 0.0;
      closed_ = !open;
      env_.start(open ? step.level : 0.0, smooth_frames_, late * into, false, shape_);
    }
    if (!closed_ && open_frames_ < sub - where.epsilon && into + where.epsilon >= open_frames_) {
      closed_ = true;
      env_.start(0.0, release_frames(), late * std::max(0.0, into - open_frames_), true, shape_);
    }
  }

  // A repeat step: slot 0 is the input as it is captured; each later slot
  // replays the capture from the step's start, at its rate and level,
  // crossfading from the slot before. The repeat before this step fades
  // out over its own crossfade.
  [[nodiscard]] Sample stutter(const Where& where, const engine::StereoFrame& in) noexcept {
    Sample y{in.left, in.right};
    const auto replay = [&](const SliceBuffer& buffer, double rate, double level, double at,
                            double start) {
      const auto frame = buffer.read(rate * at - start);
      return Sample{level * frame.left, level * frame.right};
    };
    if (plan_.active) {
      auto& buffer = buffers_[plan_.buffer];
      buffer.write(in);
      const auto slot = std::min(plan_.count - 1,
                                 static_cast<std::size_t>(std::max(0.0, where.into) / plan_.slice));
      const double at = where.into - static_cast<double>(slot) * plan_.slice;
      const auto sound = [&](std::size_t q, double t) {
        return q == 0 ? Sample{in.left, in.right}
                      : replay(buffer, plan_.rate[q], plan_.level[q], t, plan_.start);
      };
      y = sound(slot, at);
      if (slot > 0 && at < plan_.cross) {
        const Sample before = sound(slot - 1, plan_.slice + at);
        const auto gains = cross_gains(at / plan_.cross);
        y = {gains.in * y.left + gains.out * before.left,
             gains.in * y.right + gains.out * before.right};
      }
    }
    if (tail_.active) {
      if (tail_.elapsed >= tail_.cross) {
        tail_.active = false;
      } else {
        const Sample before = replay(buffers_[tail_.buffer], tail_.rate, tail_.level,
                                     tail_.slice + tail_.elapsed, tail_.start);
        const auto gains = cross_gains(tail_.elapsed / tail_.cross);
        y = {gains.in * y.left + gains.out * before.left,
             gains.in * y.right + gains.out * before.right};
        tail_.elapsed += 1.0;
      }
    }
    return y;
  }

  double rate_;
  std::array<SliceBuffer, 2> buffers_;
  PatternCapture capture_;
  std::array<StepValues, max_steps> values_{};
  int mode_ = chop_table::gate;
  std::size_t steps_ = chop_table::step_count_values[0];
  double step_beats_ = chop_table::division_beats[1];
  double smooth_frames_ = 0.0;
  double shape_ = 0.0;
  double swing_delay_ = 0.0;
  std::size_t rotate_ = 0;
  double pitch_ = 0.0;
  double decay_ = 0.0;
  std::uint32_t seed_ = 1;
  native_dsp::dsp::Ramp depth_;
  native_dsp::dsp::Ramp mix_;
  native_dsp::dsp::Ramp gate_amount_;
  native_dsp::dsp::Ramp engaged_;
  bool primed_ = false;

  // The song position: the anchor's frame and beat and the tempo since.
  Transport pending_{};
  bool pending_valid_ = false;
  bool anchored_ = false;
  bool tempo_known_ = false;
  std::int64_t anchor_frame_ = 0;
  double anchor_beat_ = 0.0;
  double bpf_ = 0.0;
  std::int64_t frame_ = 0;

  // The step playing and its gate.
  std::int64_t step_ = no_step;
  std::int64_t slot_ = 0;
  std::int64_t pattern_ = 0;
  std::size_t index_ = 0;
  bool played_ = true;
  int hit_ = -1;
  bool closed_ = true;
  bool fresh_ = true;
  double open_frames_ = 0.0;
  Transition env_;
  Plan plan_;
  Tail tail_;
  double phase_ = 0.0;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double> values, double sample_rate) {
  return std::make_unique<ChopKernel>(values, sample_rate);
}

// The gate only lowers the input. A repeat replays the step it captured,
// up to the end of the longest step its division allows and a crossfade.
std::uint64_t tail_frames(std::span<const double> values, double sample_rate) {
  if (static_cast<int>(choice(values[p::mode])) == chop_table::gate)
    return 0;
  const auto division =
      std::min(choice(values[p::division]), chop_table::division_beats.size() - 1);
  const double seconds = longest_step_seconds(chop_table::division_beats[division]) +
                         values[p::smooth] * seconds_per_millisecond;
  return static_cast<std::uint64_t>(std::ceil(seconds * sample_rate));
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const EngineDescriptor descriptor{"chop", "Chop", 1, chop_table::parameters, chop_presets::presets};
const EffectEngine chop_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

// The pattern and step are spread by two odd gammas before SplitMix64.
using native_dsp::dsp::golden_gamma;
constexpr std::uint64_t step_gamma = 0xD1B5'4A32'D192'ED03ULL;
constexpr unsigned seed_shift = 32;

} // namespace

double chance_roll(std::uint32_t seed, std::int64_t pattern, std::size_t step) noexcept {
  const std::uint64_t x = (std::uint64_t{seed} << seed_shift) ^
                          (static_cast<std::uint64_t>(pattern) * golden_gamma) ^
                          (static_cast<std::uint64_t>(step) * step_gamma);
  return native_dsp::dsp::unit_from_bits(native_dsp::dsp::splitmix64(x));
}

const EffectEngine& engine() { return chop_engine; }

} // namespace chop
} // namespace oscillate::native_dsp::effects
