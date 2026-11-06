#pragma once

#include "modulation.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/nova/analogue.hpp"
#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/project/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>

// The kernel's side of Nova Analogue (ADR-0073): seeded offsets, the
// settings a tick reads, the painted lanes compiled on control and the
// drift. Everything here is a pure function or plain data; nothing
// allocates.
namespace oscillate::native_dsp::synth::nova {

// Every seeded part: the lanes, then the parts with a tolerance and no lane,
// then the seeds drift and free-running phase draw.
enum class AnaloguePart : std::uint8_t {
  pitch,
  cutoff,
  env,
  level,
  pan,
  phase,
  mod_a,
  mod_b,
  resonance,
  shape,
  drift_phase,
  drift_phase_2,
  drift_rate,
  drift_cutoff_phase,
  free_phase,
  // Appended so the parts above keep their seeds.
  glide,
};
inline constexpr std::size_t analogue_part_count = 16;
static_assert(static_cast<std::size_t>(AnaloguePart::mod_b) + 1 == analogue_lane_count);

// A seeded value, uniform in -1..1, fixed for a seed, kind, oscillator
// (unison only), part and voice.
[[nodiscard]] double analogue_seeded(std::uint32_t seed, AnalogueKind kind, std::size_t osc,
                                     AnaloguePart part, std::size_t voice) noexcept;

// A note's re-roll unit for a part, uniform in -1..1: fixed for a seed,
// kind, oscillator (unison only), part, voice and the note's number, so
// successive notes on one card differ and a render repeats.
[[nodiscard]] double analogue_rerolled(std::uint32_t seed, AnalogueKind kind, std::size_t osc,
                                       AnaloguePart part, std::size_t voice,
                                       std::uint64_t note) noexcept;
// The seed of a voice's flutter noise: fixed for a seed, kind, oscillator,
// voice and the project frame its note starts at.
[[nodiscard]] std::uint64_t analogue_flutter_seed(std::uint32_t seed, AnalogueKind kind,
                                                  std::size_t osc, std::size_t voice,
                                                  std::uint64_t project_frame) noexcept;

// One tick's Analogue settings, from modulated values.
struct AnalogueSettings {
  bool on = false;
  bool free_running = true;
  AnalogueScope scope = AnalogueScope::notes_and_unison;
  std::uint32_t seed = 4'127;
  double age = 0.0;
  // Tolerance per part (0..1); the parts without one hold 0.
  std::array<double, analogue_part_count> tolerance{};
  double drift_amount = 0.0, drift_hz = analogue_drift_min_hz, drift_cutoff = 0.0;
  // 0: note voices by allocator slot; else this many voice cards.
  std::uint32_t cards = 0;
  double reroll = 0.0;
  double flutter_amount = 0.0, flutter_hz = analogue_flutter_tone_default_hz;
  [[nodiscard]] bool notes() const noexcept { return on && scope != AnalogueScope::unison; }
  [[nodiscard]] bool unison() const noexcept { return on && scope != AnalogueScope::notes; }
};
// `get` reads a modulated (or plain) value by table index.
template <typename Get> [[nodiscard]] AnalogueSettings analogue_settings(Get&& get) noexcept {
  const auto& a = index().analogue;
  AnalogueSettings s;
  s.on = get(a.on) >= 0.5;
  s.free_running = get(a.freerun) >= 0.5;
  s.scope = static_cast<AnalogueScope>(std::lround(get(a.scope)));
  s.seed = static_cast<std::uint32_t>(std::lround(get(a.seed)));
  s.age = get(a.age);
  const auto set = [&](AnaloguePart part, std::size_t at) {
    s.tolerance[static_cast<std::size_t>(part)] = get(at);
  };
  set(AnaloguePart::pitch, a.pitch);
  set(AnaloguePart::cutoff, a.cutoff);
  set(AnaloguePart::resonance, a.resonance);
  set(AnaloguePart::env, a.env);
  set(AnaloguePart::level, a.level);
  set(AnaloguePart::pan, a.pan);
  set(AnaloguePart::shape, a.shape);
  set(AnaloguePart::glide, a.glide);
  // Phase spreads with Age alone.
  s.tolerance[static_cast<std::size_t>(AnaloguePart::phase)] = 1.0;
  s.drift_amount = get(a.drift_amount);
  s.drift_hz =
      analogue_drift_min_hz + (analogue_drift_max_hz - analogue_drift_min_hz) * get(a.drift_rate);
  s.drift_cutoff = get(a.drift_cutoff);
  s.cards = static_cast<std::uint32_t>(
      std::clamp<long>(std::lround(get(a.cards)), 0L, std::lround(analogue_cards_max)));
  s.reroll = get(a.reroll);
  s.flutter_amount = get(a.flutter_amount);
  s.flutter_hz = get(a.flutter_tone);
  return s;
}

// A seeded unit as the voice uses it: Mod A and Mod B as drawn, the rest
// scaled by tolerance, Age and the headroom and clamped to -1..1.
[[nodiscard]] double analogue_scaled(const AnalogueSettings& settings, AnaloguePart part,
                                     double unit) noexcept;

// A part's value with this note's re-roll added: `value` (painted or
// seeded) plus reroll × the part's seeded reach × `unit`, clamped to -1..1.
// Mod A and Mod B take reroll × unit, as their seeded values are unscaled.
// Exactly `value` at Re-roll 0 or with Analogue off.
[[nodiscard]] double analogue_with_reroll(const AnalogueSettings& settings, AnaloguePart part,
                                          double value, double unit) noexcept;

// The painted lanes, compiled: NaN where a voice keeps its seeded value.
struct AnalogueEdits {
  std::array<std::array<double, analogue_note_voices>, analogue_lane_count> notes{};
  std::array<std::array<std::array<double, analogue_unison_voices>, analogue_lane_count>,
             analogue_oscillators>
      unison{};
  AnalogueEdits() noexcept;
  [[nodiscard]] static bool painted(double edit) noexcept { return !std::isnan(edit); }
};

// Nova's meaning of the document's voice lanes: known ids, lanes that reach
// the kind, voices in range, finite values in -1..1.
[[nodiscard]] foundation::Result<void>
validate_analogue_lanes(std::span<const project::NativeVoiceLane> lanes);
// For lanes validate_analogue_lanes accepted.
[[nodiscard]] AnalogueEdits compile_analogue_edits(std::span<const project::NativeVoiceLane> lanes);

// A voice's drift: two incommensurate sines on the voice's own drift clock
// (cycles), each with a seeded phase, so it wanders smoothly within -1..1.
inline constexpr double analogue_drift_ratio = 1.37;
inline constexpr double analogue_drift_weight = 0.6;
// Each voice's drift clock runs up to this much faster or slower than Rate,
// so voices never wander in step.
inline constexpr double analogue_drift_rate_spread = 0.25;
[[nodiscard]] inline double analogue_wander(double cycles, double phase_1,
                                            double phase_2) noexcept {
  constexpr double two_pi = 2.0 * std::numbers::pi;
  return analogue_drift_weight * std::sin(two_pi * (cycles + phase_1)) +
         (1.0 - analogue_drift_weight) *
             std::sin(two_pi * (analogue_drift_ratio * cycles + phase_2));
}

// Flutter: per voice, seeded white noise drawn once per control tick (so
// any block split draws the same sequence) through a one-pole low-pass at
// Tone. Normalised by the filter's steady spread so Tone changes the speed,
// not the depth, then scaled to this many standard deviations per unit and
// clamped to -1..1: the jitter rarely reaches its maximum.
inline constexpr double analogue_flutter_sigma = 0.35;
// Each generator's draws: a 64-bit LCG (Knuth's MMIX constants).
inline constexpr std::uint64_t analogue_flutter_multiplier = 6'364'136'223'846'793'005ULL;
inline constexpr std::uint64_t analogue_flutter_increment = 1'442'695'040'888'963'407ULL;
// One tick's filter: the one-pole coefficient and the factor that
// normalises its output to the configured spread.
struct FlutterTick {
  double coefficient = 0.0;
  double scale = 0.0;
};
[[nodiscard]] inline FlutterTick flutter_tick(double hz, double seconds) noexcept {
  FlutterTick tick;
  tick.coefficient = 1.0 - std::exp(-2.0 * std::numbers::pi * hz * seconds);
  // Uniform -1..1 white has variance 1/3; a one-pole with coefficient c
  // passes c / (2 - c) of it.
  constexpr double white_variance = 1.0 / 3.0;
  const double spread = std::sqrt(white_variance * tick.coefficient / (2.0 - tick.coefficient));
  tick.scale = spread > 0.0 ? analogue_flutter_sigma / spread : 0.0;
  return tick;
}
// One generator: its LCG state and its filter's output.
struct Flutter {
  std::uint64_t state = 0;
  double value = 0.0;
  // A new note: a seeded sequence starting from rest (in tune).
  void start(std::uint64_t seed) noexcept {
    state = seed;
    value = 0.0;
  }
  // Advances one control tick; returns the jitter, -1..1.
  double advance(const FlutterTick& tick) noexcept {
    state = state * analogue_flutter_multiplier + analogue_flutter_increment;
    const double white = 2.0 * hash_unit(state) - 1.0;
    value += tick.coefficient * (white - value);
    return std::clamp(value * tick.scale, -1.0, 1.0);
  }
  // The jitter now, without advancing.
  [[nodiscard]] double now(const FlutterTick& tick) const noexcept {
    return std::clamp(value * tick.scale, -1.0, 1.0);
  }
};

} // namespace oscillate::native_dsp::synth::nova
