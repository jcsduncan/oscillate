#include "oscillate/native_dsp/effects/expanse.hpp"

#include "dsp/biquad.hpp"
#include "dsp/sse2_pair.hpp"
#include "effect_dsp.hpp"
#include "expanse_presets.hpp"
#include "expanse_table.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>

namespace oscillate::native_dsp::effects::expanse {
namespace {

using expanse_table::index_of;
using expanse_table::Mode;

namespace p {
constexpr std::size_t mode = index_of("expanse.mode");
constexpr std::size_t size = index_of("expanse.size");
constexpr std::size_t decay = index_of("expanse.decay");
constexpr std::size_t predelay = index_of("expanse.predelay");
constexpr std::size_t low_decay = index_of("expanse.low_decay");
constexpr std::size_t low_freq = index_of("expanse.low_freq");
constexpr std::size_t high_decay = index_of("expanse.high_decay");
constexpr std::size_t high_freq = index_of("expanse.high_freq");
constexpr std::size_t early_diffusion = index_of("expanse.early_diffusion");
constexpr std::size_t late_diffusion = index_of("expanse.late_diffusion");
constexpr std::size_t mod_depth = index_of("expanse.mod_depth");
constexpr std::size_t mod_rate = index_of("expanse.mod_rate");
constexpr std::size_t early = index_of("expanse.early");
constexpr std::size_t late = index_of("expanse.late");
constexpr std::size_t width = index_of("expanse.width");
constexpr std::size_t low_cut = index_of("expanse.low_cut");
constexpr std::size_t high_cut = index_of("expanse.high_cut");
constexpr std::size_t shimmer = index_of("expanse.shimmer");
constexpr std::size_t shimmer_pitch = index_of("expanse.shimmer_pitch");
constexpr std::size_t freeze = index_of("expanse.freeze");
constexpr std::size_t mix = index_of("expanse.mix");
constexpr std::size_t output = index_of("expanse.output");
} // namespace p

constexpr std::size_t modes = expanse_table::modes.size();
constexpr std::size_t diffusers = 4;
constexpr std::size_t max_taps = 20;

// Each space: the network's line lengths at full size, its input
// diffusers, early reflection pattern, the in-line allpass share of each
// line and its modulation depth.
struct Profile {
  double shortest_ms;
  double longest_ms;
  std::array<double, diffusers> diffuser_ms;
  std::size_t taps;
  double first_tap_ms;
  double last_tap_ms;
  double allpass_share;
  double modulation_ms;
};

constexpr std::array<Profile, modes> profiles{{
    {24.0, 95.0, {1.7, 2.9, 4.3, 6.1}, 12, 3.0, 35.0, 0.45, 0.45},       // Room
    {30.0, 125.0, {4.7, 3.6, 12.7, 9.3}, 16, 8.0, 80.0, 0.3, 0.50},      // Hall
    {22.0, 90.0, {0.9, 1.3, 2.3, 3.1}, 0, 0.0, 0.0, 0.45, 0.25},         // Plate
    {26.0, 100.0, {2.3, 3.7, 5.9, 7.7}, 20, 2.0, 45.0, 0.42, 0.45},      // Chamber
    {50.0, 190.0, {7.1, 11.3, 17.9, 23.3}, 14, 20.0, 160.0, 0.25, 0.80}, // Cathedral
    {40.0, 160.0, {31.0, 47.0, 67.0, 89.0}, 6, 30.0, 200.0, 0.3, 2.00},  // Cloud
}};

// Lengths, the pre-delay line and early taps scale with size from a
// quarter to full.
[[nodiscard]] double size_scale(double size) noexcept { return 0.25 + 0.75 * size; }

constexpr double max_predelay_seconds = 0.5;
constexpr double max_line_seconds = 0.2;
constexpr double max_modulation_seconds = 0.003;
// Shimmer: a two-tap pitch shifter on 50 ms windows, band-limited and
// soft-limited so it can never run away.
constexpr double shimmer_window_seconds = 0.05;
constexpr double shimmer_feed = 0.7;
constexpr double meter_seconds = 0.3;
// A spilling network ends once it has fallen to -100 dBFS, or after 30 s.
constexpr double spill_floor = 1e-5;
constexpr double spill_seconds = 30.0;
// The late field is scaled so a steady input lands near its own level for
// a two-second decay.
constexpr double late_gain = 0.77;
constexpr double early_gain = 0.8;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }

// Fast Walsh-Hadamard transform, orthonormal: energy-preserving mixing.
void hadamard(std::array<double, lines>& x) noexcept {
  for (std::size_t half = 1; half < lines; half *= 2) {
    for (std::size_t i = 0; i < lines; i += 2 * half) {
      for (std::size_t j = i; j < i + half; ++j) {
        const double a = x[j];
        const double b = x[j + half];
        x[j] = a + b;
        x[j + half] = a - b;
      }
    }
  }
  for (double& value : x) {
    value *= 0.25;
  }
}

// The sign of entry i in Hadamard row r: (-1)^popcount(i & r).
[[nodiscard]] constexpr double sign(std::size_t row, std::size_t i) noexcept {
  std::size_t bits = row & i;
  int parity = 0;
  while (bits != 0) {
    parity ^= static_cast<int>(bits & 1U);
    bits >>= 1U;
  }
  return parity != 0 ? -1.0 : 1.0;
}

// The shelves that damp each line and diffuser.
using native_dsp::dsp::Biquad;

// A Schroeder allpass on a fractional delay, read through an allpass
// interpolator so it loses nothing in the loop. With `damping`, what
// circulates inside loses `gain` and the shelf on every pass, so energy held
// here decays by the space's own band rates.
struct Allpass {
  dsp::Delay line;
  dsp::AllpassReader reader;
  [[nodiscard]] double process(double x, double frames, double g, double gain = 1.0,
                               Biquad* damping = nullptr) noexcept {
    double delayed = reader.read(line, std::max(2.0, frames) - 1.0);
    if (damping != nullptr) {
      delayed = damping->process(delayed * gain);
    }
    const double v = x + g * delayed;
    line.write(v);
    return delayed - g * v;
  }
};

struct Tap {
  double ms;
  double gain;
  bool cross; // Read from the other channel.
};

// Deterministic per-space early reflection patterns: denser toward the
// end, falling off, alternating sign, a quarter of them crossing sides.
struct Pattern {
  std::array<std::array<Tap, max_taps>, 2> taps{};
  std::size_t count = 0;
};

[[nodiscard]] Pattern make_pattern(const Profile& profile, std::uint32_t seed) {
  Pattern pattern;
  pattern.count = profile.taps;
  if (pattern.count == 0) {
    return pattern;
  }
  std::uint32_t state = seed;
  const auto next = [&] {
    state = state * 1'664'525U + 1'013'904'223U;
    return static_cast<double>(state >> 8U) / 16'777'216.0;
  };
  for (std::size_t c = 0; c < 2; ++c) {
    double energy = 0.0;
    for (std::size_t k = 0; k < pattern.count; ++k) {
      const double position =
          (static_cast<double>(k) + 0.5 * next()) / static_cast<double>(pattern.count);
      const double ms = profile.first_tap_ms +
                        (profile.last_tap_ms - profile.first_tap_ms) * std::pow(position, 1.4);
      const double gain = std::pow(0.82, static_cast<double>(k)) * (0.7 + 0.3 * next()) *
                          ((k + c) % 2 == 0 ? 1.0 : -1.0);
      pattern.taps[c][k] = {ms, gain, next() < 0.25};
      energy += gain * gain;
    }
    for (std::size_t k = 0; k < pattern.count; ++k) {
      pattern.taps[c][k].gain /= std::sqrt(energy);
    }
  }
  return pattern;
}

// Settings every network reads each tick.
struct Settings {
  double size = 0.6;
  double decay = 2.4;
  double low_decay = 1.0;
  double low_freq = 300.0;
  double high_decay = 1.0;
  double high_freq = 4'500.0;
  double early_diffusion = 0.7;
  double late_diffusion = 0.6;
  double mod_depth = 0.0;
  double mod_rate = 0.6;
  bool freeze = false;
  friend bool operator==(const Settings&, const Settings&) = default;
};

// A unit phasor: cos and sin of an angle.
struct Phasor {
  double cos = 1.0;
  double sin = 0.0;
};

// `p` turned on by `turn`.
[[nodiscard]] Phasor rotated(Phasor p, Phasor turn) noexcept {
  return {p.cos * turn.cos - p.sin * turn.sin, p.sin * turn.cos + p.cos * turn.sin};
}

// Two lines (or two channels) at once, each lane exactly its scalar
// counterpart's arithmetic: see dsp::Pair.
using native_dsp::dsp::Pair;
static_assert(lines % 2 == 0);

// AllpassReader::read on two lines.
[[gnu::always_inline]] [[nodiscard]] inline Pair
read_pair(const dsp::Delay& first_line, dsp::AllpassReader& first_reader, double first_frames,
          const dsp::Delay& second_line, dsp::AllpassReader& second_reader,
          double second_frames) noexcept {
  const auto split = [](double frames, std::size_t& whole) {
    frames = frames < 1.0 ? 1.0 : frames;
    whole = static_cast<std::size_t>(frames - 0.5);
    return frames - static_cast<double>(whole);
  };
  std::size_t first_whole = 0;
  std::size_t second_whole = 0;
  const Pair fraction(split(first_frames, first_whole), split(second_frames, second_whole));
  const Pair eta = (1.0 - fraction) / (1.0 + fraction);
  const Pair y = eta * Pair(first_line.sample(first_whole), second_line.sample(second_whole)) +
                 Pair(first_line.sample(first_whole + 1), second_line.sample(second_whole + 1)) -
                 eta * Pair(first_reader.previous, second_reader.previous);
  first_reader.previous = y.first();
  second_reader.previous = y.second();
  return y;
}

// Biquad::process on two filters.
[[gnu::always_inline]] [[nodiscard]] inline Pair biquad_pair(Biquad& first, Biquad& second,
                                                             Pair x) noexcept {
  const Pair y = Pair(first.b0, second.b0) * x + Pair(first.z1, second.z1);
  const Pair z1 =
      Pair(first.b1, second.b1) * x - Pair(first.a1, second.a1) * y + Pair(first.z2, second.z2);
  const Pair z2 = Pair(first.b2, second.b2) * x - Pair(first.a2, second.a2) * y;
  first.z1 = z1.first();
  second.z1 = z1.second();
  first.z2 = z2.first();
  second.z2 = z2.second();
  return y;
}

// Allpass::process on two allpasses with the same g.
[[gnu::always_inline]] [[nodiscard]] inline Pair
allpass_pair(Allpass& first, Allpass& second, Pair x, double first_frames, double second_frames,
             double g, Pair gain = Pair::both(1.0), Biquad* first_damping = nullptr,
             Biquad* second_damping = nullptr) noexcept {
  Pair delayed = read_pair(first.line, first.reader, std::max(2.0, first_frames) - 1.0, second.line,
                           second.reader, std::max(2.0, second_frames) - 1.0);
  if (first_damping != nullptr && second_damping != nullptr) {
    delayed = biquad_pair(*first_damping, *second_damping, delayed * gain);
  }
  const Pair v = x + g * delayed;
  first.line.write(v.first());
  second.line.write(v.second());
  return delayed - g * v;
}

// Frames between exact evaluations of the wander's sines.
constexpr std::uint32_t wander_anchor = 32;

// One space's diffusion and late field.
class Network {
public:
  void prepare(double rate) {
    rate_ = rate;
    derived_ = false;
    const auto line_frames = static_cast<std::size_t>(
        std::ceil((max_line_seconds * (1.0 + 0.1) + max_modulation_seconds) * rate));
    for (auto& line : lines_) {
      line.prepare(line_frames);
    }
    for (auto& pair : loop_allpasses_) {
      for (auto& allpass : pair) {
        allpass.line.prepare(static_cast<std::size_t>(std::ceil(max_line_seconds * 0.4 * rate)));
      }
    }
    for (auto& channel : diffusers_) {
      for (auto& allpass : channel) {
        allpass.line.prepare(static_cast<std::size_t>(std::ceil(0.1 * rate)));
      }
    }
  }

  void configure(int mode) noexcept { mode_ = mode; }
  [[nodiscard]] int mode() const noexcept { return mode_; }

  void reset() noexcept {
    for (auto& line : lines_) {
      line.reset();
    }
    readers_.fill({});
    for (auto& pair : loop_allpasses_) {
      for (auto& allpass : pair) {
        allpass.line.reset();
        allpass.reader = {};
      }
    }
    for (auto& channel : diffusers_) {
      for (auto& allpass : channel) {
        allpass.line.reset();
        allpass.reader = {};
      }
    }
    for (auto& channel : diffuser_damping_) {
      for (auto& filter : channel) {
        filter.reset();
      }
    }
    for (auto& filter : low_) {
      filter.reset();
    }
    for (auto& filter : high_) {
      filter.reset();
    }
    // The wander restarts too, so a reset network plays exactly as a new
    // one: a seek renders what an export from there renders.
    phase_a_.fill(0.0);
    phase_b_.fill(0.0);
    level_ = 0.0;
    until_anchor_ = 0;
  }

  // Control: line lengths, allpass lengths and absorption for these
  // settings. Lengths glide per sample toward the new ones.
  void control(const Settings& s, std::uint32_t frames, bool jump) noexcept {
    // What the settings give is derived again only when they or the space
    // change: the same values, without the powers, shelves and sines of
    // every tick.
    if (!derived_ || derived_mode_ != mode_ || !(derived_settings_ == s)) {
      derive(s);
      derived_ = true;
      derived_mode_ = mode_;
      derived_settings_ = s;
    }
    const double steps = std::max<double>(1.0, frames);
    for (std::size_t i = 0; i < lines; ++i) {
      length_[i].to(line_target_[i], steps, jump);
      allpass_length_[i].to(allpass_target_[i], steps, jump);
    }
    until_anchor_ = 0;
  }

  struct Out {
    std::array<double, lines> taps{};
    double left = 0.0;
    double right = 0.0;
  };

private:
  // Everything control() sets from the settings and the space alone.
  void derive(const Settings& s) noexcept {
    const auto& profile = profiles[static_cast<std::size_t>(mode_)];
    const double scale = size_scale(s.size);
    const double ms = rate_ * 1e-3;
    for (std::size_t i = 0; i < lines; ++i) {
      // Log-spaced, with an irregular jitter so no two lines line up.
      const double position = static_cast<double>(i) / static_cast<double>(lines - 1);
      const double jitter = 1.0 + 0.07 * std::sin(2.39996 * static_cast<double>(i + 1));
      const double length = profile.shortest_ms *
                            std::pow(profile.longest_ms / profile.shortest_ms, position) * jitter *
                            scale * ms;
      const double allpass = length * profile.allpass_share;
      // The allpass's mean delay is its length: it shares the line's budget.
      line_target_[i] = std::max(4.0 + profile.modulation_ms * ms, length - allpass);
      allpass_target_[i] = std::max(2.0, allpass);
      // Jot absorption: each band loses 60 dB over its RT60 for this loop,
      // the allpass's (mean) delay included.
      const double loop_seconds = length / rate_;
      const double mid = s.freeze ? 1.0 : std::pow(10.0, -3.0 * loop_seconds / s.decay);
      const double low =
          s.freeze ? 1.0 : std::pow(10.0, -3.0 * loop_seconds / (s.decay * s.low_decay));
      const double high =
          s.freeze ? 1.0 : std::pow(10.0, -3.0 * loop_seconds / (s.decay * s.high_decay));
      mid_gain_[i] = mid;
      low_[i].set(native_dsp::dsp::rbj_shelf(false, low / mid, s.low_freq, rate_));
      high_[i].set(native_dsp::dsp::rbj_shelf(true, high / mid, s.high_freq, rate_));
      // Two incommensurate sines per line: slow, quasi-random wander.
      const double spread = 0.6 + 0.8 * dsp::wrap(0.618034 * static_cast<double>(i + 1));
      rate_a_[i] = s.mod_rate * spread / rate_;
      rate_b_[i] = s.mod_rate * spread * 2.73 / rate_;
      const double turn_a = 2.0 * std::numbers::pi * rate_a_[i];
      const double turn_b = 2.0 * std::numbers::pi * rate_b_[i];
      turn_a_[i] = {std::cos(turn_a), std::sin(turn_a)};
      turn_b_[i] = {std::cos(turn_b), std::sin(turn_b)};
    }
    for (std::size_t c = 0; c < 2; ++c) {
      for (std::size_t k = 0; k < diffusers; ++k) {
        const double frames_k =
            profile.diffuser_ms[k] * (c == 0 ? 1.0 : 1.07) * (0.5 + 0.5 * s.size) * ms;
        diffuser_length_[c][k] = frames_k;
        const double seconds = frames_k / rate_;
        const double mid = s.freeze ? 1.0 : std::pow(10.0, -3.0 * seconds / s.decay);
        const double high =
            s.freeze ? 1.0 : std::pow(10.0, -3.0 * seconds / (s.decay * s.high_decay));
        diffuser_gain_[c][k] = mid;
        diffuser_damping_[c][k].set(
            native_dsp::dsp::rbj_shelf(true, high / mid, s.high_freq, rate_));
      }
    }
    modulation_frames_ = profile.modulation_ms * s.mod_depth * ms;
    input_g_ = 0.75 * s.early_diffusion;
    loop_g_ = 0.6 * s.late_diffusion;
  }

public:
  // One frame: `left`/`right` is the diffused input, `extra` more input
  // per line (early reflections, shimmer).
  void process(double left, double right, const std::array<double, lines>& extra,
               Out& out) noexcept {
    // Input diffusion: both channels through their chains at once.
    Pair diffused_in(left, right);
    for (std::size_t k = 0; k < diffusers; ++k) {
      diffused_in = allpass_pair(diffusers_[0][k], diffusers_[1][k], diffused_in,
                                 diffuser_length_[0][k], diffuser_length_[1][k], input_g_,
                                 Pair(diffuser_gain_[0][k], diffuser_gain_[1][k]),
                                 &diffuser_damping_[0][k], &diffuser_damping_[1][k]);
    }
    const std::array<double, 2> in{diffused_in.first(), diffused_in.second()};
    // Read every line, through its allpass and absorption: two lines at once.
    std::array<double, lines> a{};
    // The wander's sines: exact every wander_anchor frames, rotated by
    // each line's per-frame turn between (the same sines to within a few
    // ulps, without 32 sine calls a frame).
    const bool anchor = until_anchor_ == 0;
    until_anchor_ = anchor ? wander_anchor - 1 : until_anchor_ - 1;
    for (std::size_t i = 0; i < lines; i += 2) {
      std::array<double, 2> length{};
      std::array<double, 2> share{};
      for (std::size_t k = 0; k < 2; ++k) {
        const std::size_t line = i + k;
        phase_a_[line] = dsp::wrap(phase_a_[line] + rate_a_[line]);
        phase_b_[line] = dsp::wrap(phase_b_[line] + rate_b_[line]);
        if (anchor) {
          const double angle_a =
              2.0 * std::numbers::pi * (phase_a_[line] + static_cast<double>(line) / lines);
          const double angle_b =
              2.0 * std::numbers::pi * (phase_b_[line] + 0.37 * static_cast<double>(line));
          wander_a_[line] = {std::cos(angle_a), std::sin(angle_a)};
          wander_b_[line] = {std::cos(angle_b), std::sin(angle_b)};
        } else {
          wander_a_[line] = rotated(wander_a_[line], turn_a_[line]);
          wander_b_[line] = rotated(wander_b_[line], turn_b_[line]);
        }
        const double wander = 0.7 * wander_a_[line].sin + 0.3 * wander_b_[line].sin;
        // The wander has zero mean, so the loop keeps its designed length.
        length[k] = length_[line].next() + modulation_frames_ * wander;
        share[k] = allpass_length_[line].next();
      }
      const std::size_t j = i + 1;
      const Pair read = read_pair(lines_[i], readers_[i], std::max(2.0, length[0]) - 1.0, lines_[j],
                                  readers_[j], std::max(2.0, length[1]) - 1.0);
      // Two allpasses of 62% and 38% of the line's allpass share.
      const Pair first = allpass_pair(loop_allpasses_[i][0], loop_allpasses_[j][0], read,
                                      0.62 * share[0], 0.62 * share[1], loop_g_);
      const Pair diffused = allpass_pair(loop_allpasses_[i][1], loop_allpasses_[j][1], first,
                                         0.38 * share[0], 0.38 * share[1], -loop_g_);
      const Pair absorbed =
          biquad_pair(high_[i], high_[j],
                      biquad_pair(low_[i], low_[j], diffused * Pair(mid_gain_[i], mid_gain_[j])));
      a[i] = absorbed.first();
      a[j] = absorbed.second();
    }
    // Stereo outputs from two orthogonal rows.
    double l = 0.0;
    double r = 0.0;
    for (std::size_t i = 0; i < lines; ++i) {
      l += sign(1, i) * a[i];
      r += sign(2, i) * a[i];
    }
    out.left = 0.25 * l;
    out.right = 0.25 * r;
    out.taps = a;
    // Mix and feed back with the input on two further rows.
    std::array<double, lines> v = a;
    hadamard(v);
    for (std::size_t i = 0; i < lines; ++i) {
      lines_[i].write(v[i] + 0.25 * (sign(3, i) * in[0] + sign(5, i) * in[1]) + extra[i]);
    }
    level_ = std::max(std::fabs(out.left), std::max(std::fabs(out.right), level_ * 0.9999));
  }

  [[nodiscard]] double level() const noexcept { return level_; }

private:
  // derive()'s inputs, and the line and allpass lengths it gave.
  bool derived_ = false;
  int derived_mode_ = Mode::hall;
  Settings derived_settings_{};
  std::array<double, lines> line_target_{};
  std::array<double, lines> allpass_target_{};
  double rate_ = 48'000.0;
  int mode_ = Mode::hall;
  std::array<dsp::Delay, lines> lines_;
  std::array<dsp::AllpassReader, lines> readers_{};
  std::array<std::array<Allpass, 2>, lines> loop_allpasses_;
  std::array<std::array<Allpass, diffusers>, 2> diffusers_;
  std::array<std::array<double, diffusers>, 2> diffuser_length_{};
  std::array<std::array<double, diffusers>, 2> diffuser_gain_{};
  std::array<std::array<Biquad, diffusers>, 2> diffuser_damping_{};
  std::array<dsp::Ramp, lines> length_{};
  std::array<dsp::Ramp, lines> allpass_length_{};
  std::array<double, lines> mid_gain_{};
  std::array<Biquad, lines> low_{};
  std::array<Biquad, lines> high_{};
  std::array<double, lines> phase_a_{};
  std::array<double, lines> phase_b_{};
  std::array<double, lines> rate_a_{};
  std::array<double, lines> rate_b_{};
  // Each line's two wander sines (with their cosines) and their turns per
  // frame; frames until the next exact anchor.
  std::array<Phasor, lines> wander_a_{};
  std::array<Phasor, lines> wander_b_{};
  std::array<Phasor, lines> turn_a_{};
  std::array<Phasor, lines> turn_b_{};
  std::uint32_t until_anchor_ = 0;
  double modulation_frames_ = 0.0;
  double input_g_ = 0.0;
  double loop_g_ = 0.0;
  double level_ = 0.0;
};

// A two-tap windowed pitch shifter.
class PitchShifter {
public:
  void prepare(double rate) {
    window_ = shimmer_window_seconds * rate;
    line_.prepare(static_cast<std::size_t>(std::ceil(window_ * 2.0)) + 8);
  }
  void reset() noexcept {
    line_.reset();
    phase_ = 0.0;
  }
  void set_ratio(double ratio) noexcept { increment_ = (ratio - 1.0) / window_; }
  [[nodiscard]] double process(double x) noexcept {
    line_.write(x);
    phase_ = dsp::wrap(phase_ + increment_);
    double y = 0.0;
    for (const double offset : {0.0, 0.5}) {
      const double phase = dsp::wrap(phase_ + offset);
      // Delay falls through the window for upward shifts, rises for down.
      const double delay = 2.0 + window_ * (1.0 - phase);
      const double weight = std::sin(std::numbers::pi * phase);
      y += weight * weight * line_.read(delay);
    }
    return y;
  }

private:
  dsp::Delay line_;
  double window_ = 2'400.0;
  double phase_ = 0.0;
  double increment_ = 0.0;
};

class ExpanseKernel final : public EffectKernel {
public:
  explicit ExpanseKernel(double sample_rate)
      : rate_(sample_rate), meter_(1.0 - std::exp(-1.0 / (meter_seconds * sample_rate))),
        spill_frames_(static_cast<std::uint64_t>(spill_seconds * sample_rate)),
        switch_step_(1.0 / (0.02 * sample_rate)) {
    const double longest_tap = 200.0 * 1e-3;
    const auto predelay_frames =
        static_cast<std::size_t>(std::ceil((max_predelay_seconds + longest_tap) * sample_rate)) + 8;
    for (auto& line : predelay_) {
      line.prepare(predelay_frames);
    }
    for (auto& network : networks_) {
      network.prepare(sample_rate);
    }
    for (std::size_t m = 0; m < modes; ++m) {
      patterns_[m] = make_pattern(profiles[m], static_cast<std::uint32_t>(91 + 37 * m));
    }
    shifter_.prepare(sample_rate);
    reset();
  }

  void control(std::span<const double> v, std::uint32_t frames,
               const SynthTransport&) noexcept override {
    const int mode = as_int(v[p::mode]);
    const bool jump = !primed_;
    if (jump) {
      networks_[active_].configure(mode);
      early_mode_ = mode;
      fading_early_mode_ = mode;
    } else if (mode != networks_[active_].mode()) {
      switch_to(mode);
    }
    settings_.size = v[p::size];
    settings_.decay = v[p::decay];
    settings_.low_decay = v[p::low_decay];
    settings_.low_freq = v[p::low_freq];
    settings_.high_decay = v[p::high_decay];
    settings_.high_freq = v[p::high_freq];
    settings_.early_diffusion = v[p::early_diffusion];
    settings_.late_diffusion = v[p::late_diffusion];
    settings_.mod_depth = v[p::mod_depth];
    settings_.mod_rate = v[p::mod_rate];
    settings_.freeze = v[p::freeze] != 0.0;
    for (std::size_t n = 0; n < networks_.size(); ++n) {
      if (n == active_ || spilling_[n]) {
        networks_[n].control(settings_, frames, jump);
      }
    }
    const double steps = std::max<double>(1.0, frames);
    predelay_frames_.to(v[p::predelay] * 1e-3 * rate_, steps, jump);
    scale_.to(size_scale(v[p::size]), steps, jump);
    early_.to(v[p::early] * early_gain, steps, jump);
    late_.to(v[p::late] * late_gain, steps, jump);
    width_.to(v[p::width], steps, jump);
    // Freeze holds the tail and closes the input; shimmer would run away.
    input_.to(settings_.freeze ? 0.0 : 1.0, steps, jump);
    shimmer_.to(settings_.freeze ? 0.0 : v[p::shimmer] * shimmer_feed, steps, jump);
    mix_.to(v[p::mix], steps, jump);
    gain_.to(dsp::db_to_gain(v[p::output]), steps, jump);
    shifter_.set_ratio(std::exp2(
        expanse_table::interval_semitones[static_cast<std::size_t>(v[p::shimmer_pitch])] / 12.0));
    low_cut_ = synth::make_filter_coefficients(synth::FilterMode::highpass_12,
                                               v[p::low_cut] / rate_, 0.0, 1.0);
    high_cut_ = synth::make_filter_coefficients(synth::FilterMode::lowpass_12,
                                                v[p::high_cut] / rate_, 0.0, 1.0);
    shimmer_low_ =
        synth::make_filter_coefficients(synth::FilterMode::highpass_12, 200.0 / rate_, 0.0, 1.0);
    shimmer_high_ =
        synth::make_filter_coefficients(synth::FilterMode::lowpass_12, 8'000.0 / rate_, 0.0, 1.0);
    primed_ = true;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    for (auto& frame : audio) {
      const std::array<double, 2> x{frame.left, frame.right};
      const double input = input_.next();
      const double predelay = predelay_frames_.next();
      const double scale = scale_.next();
      const double early_level = early_.next();
      const double late_level = late_.next();
      const double width = width_.next();
      const double shimmer = shimmer_.next();
      const double mix = mix_.next();
      const double gain = gain_.next();
      for (std::size_t c = 0; c < 2; ++c) {
        predelay_[c].write(x[c] * input);
      }
      // Pre-delayed input, and early reflections from the space's pattern
      // (crossfading for 20 ms after a change of space).
      std::array<double, 2> delayed{};
      for (std::size_t c = 0; c < 2; ++c) {
        delayed[c] = predelay_[c].read(std::max(1.0, predelay));
      }
      std::array<double, 2> reflections = cached_reflections(predelay, scale);
      if (early_fade_ < 1.0) {
        const auto old = early_reflections(fading_early_mode_, predelay, scale);
        for (std::size_t c = 0; c < 2; ++c) {
          reflections[c] = old[c] + (reflections[c] - old[c]) * early_fade_;
        }
        early_fade_ = std::min(1.0, early_fade_ + switch_step_);
      }
      // Late field: the active network takes the input, reflections and
      // shimmer; a spilling one rings out on silence.
      std::array<double, lines> extra{};
      const double shimmered = shimmer > 0.0 ? shimmer_sample(shimmer) : 0.0;
      for (std::size_t i = 0; i < lines; ++i) {
        extra[i] = 0.125 * (sign(6, i) * reflections[0] + sign(9, i) * reflections[1]) +
                   0.25 * sign(10, i) * shimmered;
      }
      double late_left = 0.0;
      double late_right = 0.0;
      for (std::size_t n = 0; n < networks_.size(); ++n) {
        if (n == active_) {
          networks_[n].process(delayed[0], delayed[1], extra, scratch_);
          shimmer_source_ = 0.0;
          for (std::size_t i = 0; i < lines; ++i) {
            shimmer_source_ += sign(12, i) * scratch_.taps[i];
          }
          shimmer_source_ *= 0.25;
          late_left += scratch_.left;
          late_right += scratch_.right;
        } else if (spilling_[n]) {
          networks_[n].process(0.0, 0.0, silence_, scratch_);
          const double fade = spill_fade_[n];
          late_left += scratch_.left * fade;
          late_right += scratch_.right * fade;
          ++spill_age_[n];
          if (spill_cut_[n] || spill_age_[n] > spill_frames_) {
            spill_fade_[n] = std::max(0.0, fade - switch_step_);
          }
          if (spill_fade_[n] <= 0.0 || networks_[n].level() < spill_floor) {
            spilling_[n] = false;
          }
        }
      }
      // Wet: early plus late, width, then the output cuts.
      std::array<double, 2> wet{early_level * reflections[0] + late_level * late_left,
                                early_level * reflections[1] + late_level * late_right};
      const double mid = 0.5 * (wet[0] + wet[1]);
      const double side = 0.5 * (wet[0] - wet[1]) * width;
      wet = {mid + side, mid - side};
      for (std::size_t c = 0; c < 2; ++c) {
        wet[c] =
            high_cut_filter_[c].process(low_cut_filter_[c].process(wet[c], low_cut_), high_cut_);
      }
      // Meters.
      const double in_power = 0.5 * (x[0] * x[0] + x[1] * x[1]);
      input_power_ += meter_ * (in_power - input_power_);
      const double early_power =
          0.5 * early_level * early_level *
          (reflections[0] * reflections[0] + reflections[1] * reflections[1]);
      early_power_ += meter_ * (early_power - early_power_);
      const double late_power =
          0.5 * late_level * late_level * (late_left * late_left + late_right * late_right);
      late_power_ += meter_ * (late_power - late_power_);
      frame.left = static_cast<float>((x[0] + (wet[0] - x[0]) * mix) * gain);
      frame.right = static_cast<float>((x[1] + (wet[1] - x[1]) * mix) * gain);
    }
  }

  void reset() noexcept override {
    for (auto& line : predelay_) {
      line.reset();
    }
    for (std::size_t n = 0; n < networks_.size(); ++n) {
      networks_[n].reset();
      spilling_[n] = false;
    }
    for (std::size_t c = 0; c < 2; ++c) {
      low_cut_filter_[c].reset();
      high_cut_filter_[c].reset();
    }
    shimmer_low_filter_.reset();
    shimmer_high_filter_.reset();
    shifter_.reset();
    shimmer_source_ = 0.0;
    early_fade_ = 1.0;
    input_power_ = 0.0;
    early_power_ = 0.0;
    late_power_ = 0.0;
    primed_ = false; // The next block jumps to its settings, as a new kernel does.
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    out[telemetry::input_rms] = static_cast<float>(std::sqrt(input_power_));
    out[telemetry::early_rms] = static_cast<float>(std::sqrt(early_power_));
    out[telemetry::late_rms] = static_cast<float>(std::sqrt(late_power_));
    return telemetry::size;
  }

private:
  // A change of space: the new space takes over in the free network; the
  // old one keeps ringing, without input, until it fades out. If both are
  // busy the older spill is cut with a short fade first.
  void switch_to(int mode) noexcept {
    const std::size_t next = 1 - active_;
    if (spilling_[next]) {
      // Cut the previous spill before reusing its network.
      spill_cut_[next] = true;
      if (spill_fade_[next] > 0.0) {
        return; // Try again next tick, once the fade has run.
      }
    }
    networks_[next].reset();
    networks_[next].configure(mode);
    networks_[next].control(settings_, control_interval_frames, true);
    spilling_[active_] = true;
    spill_cut_[active_] = false;
    spill_fade_[active_] = 1.0;
    spill_age_[active_] = 0;
    spilling_[next] = false;
    active_ = next;
    fading_early_mode_ = early_mode_;
    early_mode_ = mode;
    early_fade_ = 0.0;
  }

  // early_reflections() for the current space, its read positions kept
  // while the pre-delay and size hold still (the same reads, the same
  // weights).
  [[nodiscard]] std::array<double, 2> cached_reflections(double predelay, double scale) noexcept {
    const auto& pattern = patterns_[static_cast<std::size_t>(early_mode_)];
    if (early_mode_ != cached_mode_ || predelay != cached_predelay_ || scale != cached_scale_) {
      cached_mode_ = early_mode_;
      cached_predelay_ = predelay;
      cached_scale_ = scale;
      const double ms = rate_ * 1e-3;
      for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t k = 0; k < pattern.count; ++k) {
          const auto& tap = pattern.taps[c][k];
          const auto& line = predelay_[tap.cross ? 1 - c : c];
          early_taps_[c][k] = line.tap(std::max(1.0, predelay + tap.ms * scale * ms));
        }
      }
    }
    std::array<double, 2> out{};
    for (std::size_t c = 0; c < 2; ++c) {
      for (std::size_t k = 0; k < pattern.count; ++k) {
        const auto& tap = pattern.taps[c][k];
        const auto& line = predelay_[tap.cross ? 1 - c : c];
        out[c] += tap.gain * line.read(early_taps_[c][k]);
      }
    }
    return out;
  }

  [[nodiscard]] std::array<double, 2> early_reflections(int mode, double predelay,
                                                        double scale) const noexcept {
    const auto& pattern = patterns_[static_cast<std::size_t>(mode)];
    std::array<double, 2> out{};
    const double ms = rate_ * 1e-3;
    for (std::size_t c = 0; c < 2; ++c) {
      for (std::size_t k = 0; k < pattern.count; ++k) {
        const auto& tap = pattern.taps[c][k];
        const auto& line = predelay_[tap.cross ? 1 - c : c];
        out[c] += tap.gain * line.read(std::max(1.0, predelay + tap.ms * scale * ms));
      }
    }
    return out;
  }

  // The shimmer path: last frame's network output, pitch-shifted, band-
  // limited and soft-limited.
  [[nodiscard]] double shimmer_sample(double amount) noexcept {
    double y = shifter_.process(shimmer_source_);
    y = shimmer_high_filter_.process(shimmer_low_filter_.process(y, shimmer_low_), shimmer_high_);
    return amount * std::tanh(y);
  }

  double rate_;
  double meter_;
  std::uint64_t spill_frames_;
  double switch_step_;
  std::array<dsp::Delay, 2> predelay_;
  std::array<Network, 2> networks_;
  std::array<bool, 2> spilling_{};
  std::array<bool, 2> spill_cut_{};
  std::array<double, 2> spill_fade_{};
  std::array<std::uint64_t, 2> spill_age_{};
  std::size_t active_ = 0;
  std::array<Pattern, modes> patterns_{};
  int early_mode_ = Mode::hall;
  int fading_early_mode_ = Mode::hall;
  // cached_reflections()' read positions and what they were taken for.
  std::array<std::array<dsp::Delay::Tap, max_taps>, 2> early_taps_{};
  int cached_mode_ = -1;
  double cached_predelay_ = 0.0;
  double cached_scale_ = 0.0;
  double early_fade_ = 1.0;
  Settings settings_;
  Network::Out scratch_{};
  std::array<double, lines> silence_{};
  PitchShifter shifter_;
  synth::MultimodeFilter shimmer_low_filter_;
  synth::MultimodeFilter shimmer_high_filter_;
  synth::FilterCoefficients shimmer_low_{};
  synth::FilterCoefficients shimmer_high_{};
  double shimmer_source_ = 0.0;
  std::array<synth::MultimodeFilter, 2> low_cut_filter_{};
  std::array<synth::MultimodeFilter, 2> high_cut_filter_{};
  synth::FilterCoefficients low_cut_{};
  synth::FilterCoefficients high_cut_{};
  dsp::Ramp predelay_frames_;
  dsp::Ramp scale_;
  dsp::Ramp early_;
  dsp::Ramp late_;
  dsp::Ramp width_;
  dsp::Ramp input_;
  dsp::Ramp shimmer_;
  dsp::Ramp mix_;
  dsp::Ramp gain_;
  double input_power_ = 0.0;
  double early_power_ = 0.0;
  double late_power_ = 0.0;
  bool primed_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<ExpanseKernel>(sample_rate);
}

// The slowest band's RT60 with half again for the build-up, more for
// shimmer's extra loop gain, plus pre-delay, reflections and diffusion.
// Freeze sustains indefinitely and is held to 59 seconds.
std::uint64_t tail_frames(std::span<const double> v, double sample_rate) {
  if (v[p::freeze] != 0.0) {
    return static_cast<std::uint64_t>(59.0 * sample_rate);
  }
  const double decay = v[p::decay];
  const double slowest = decay * std::max({1.0, v[p::low_decay], v[p::high_decay]});
  const double seconds =
      slowest * 1.5 * (1.0 + 2.0 * v[p::shimmer]) + v[p::predelay] * 1e-3 + 0.2 + 0.4;
  return static_cast<std::uint64_t>(std::ceil(std::min(seconds, 59.0) * sample_rate));
}

std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"expanse", "Expanse", 1, expanse_table::parameters,
                                         expanse_presets::presets};
const EffectEngine expanse_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames,
                                  false};

} // namespace

const EffectEngine& engine() { return expanse_engine; }

} // namespace oscillate::native_dsp::effects::expanse
