#pragma once

#include "oscillate/native_dsp/synth/nova/parameters.hpp"
#include "oscillate/project/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Nova's modulation (spec 08 §6, contract 08a §3 and §4): curve shapes,
// envelopes, drawn LFOs, chaos and the route matrix. Control-tick rate
// unless noted; nothing here allocates after construction.
namespace oscillate::native_dsp::synth::nova {

// 08a §3.2: the one bend every curve, route and envelope segment uses.
[[nodiscard]] inline double bend(double t, double tension) noexcept {
  t = std::clamp(t, 0.0, 1.0);
  if (tension > 0.0)
    return std::pow(t, 1.0 + 4.0 * tension);
  if (tension < 0.0)
    return 1.0 - std::pow(1.0 - t, 1.0 - 4.0 * tension);
  return t;
}

// A drawn curve sampled once on control, read with linear interpolation.
// Steps are exact to 1/curve_resolution of a cycle.
inline constexpr std::size_t curve_resolution = 2'048;
// Steps a cycle that a Free LFO's song position is snapped to (2^32).
inline constexpr double song_cycle_grid = 4'294'967'296.0;
struct CurveShape {
  std::array<float, curve_resolution + 1> y{};
  [[nodiscard]] double at(double phase) const noexcept {
    phase -= std::floor(phase);
    const double x = phase * curve_resolution;
    const auto i = std::min(static_cast<std::size_t>(x), curve_resolution - 1);
    const double t = x - static_cast<double>(i);
    return y[i] + t * (y[i + 1] - y[i]);
  }
};
// Samples a document curve; points must already be valid (model checked).
[[nodiscard]] CurveShape sample_curve(std::span<const project::NativeCurvePoint> points);
// 08a §3.2 defaults: the LFO triangle and the identity remap.
[[nodiscard]] const CurveShape& default_lfo_shape();
[[nodiscard]] const CurveShape& identity_shape();

// 08a §4, in this order. The telemetry block at offset 35..68 follows the
// same order for LFOs, macros and the rest; the voice sources Analogue
// added (ADR-0073) publish at 2016..2018 and the arp's (ADR-0078) at
// 2035..2036.
enum class Source : std::uint8_t {
  env1,
  env2,
  env3,
  env4,
  lfo1,
  lfo2,
  lfo3,
  lfo4,
  lfo5,
  lfo6,
  lfo7,
  lfo8,
  macro1,
  macro2,
  macro3,
  macro4,
  macro5,
  macro6,
  macro7,
  macro8,
  velocity,
  note,
  modwheel,
  aftertouch,
  mpe_slide,
  mpe_pressure,
  chaos1,
  chaos2,
  random,
  bend,
  voice_mod_a,
  voice_mod_b,
  voice_spread,
  arp_mod,
  arp_accent,
};
inline constexpr std::size_t source_count = 35;
// voice.spread for a note voice, which has no place in a unison stack.
inline constexpr double voice_spread_centre = 0.5;
[[nodiscard]] std::optional<Source> source_from_id(std::string_view id) noexcept;

// Each source's unipolar value s (0..1) and a gain the route signal is
// scaled by (an LFO's rise; 1 otherwise).
struct SourceValues {
  std::array<double, source_count> value{};
  std::array<double, source_count> gain{};
  SourceValues() { gain.fill(1.0); }
};

// A route compiled against the table: destination as an index, sources as
// enums. aux_none marks a route with no aux.
inline constexpr std::uint8_t aux_none = 0xFF;
struct CompiledRoute {
  std::uint16_t slot = 0;
  std::uint16_t destination = 0; // Table index of a continuous parameter.
  std::uint8_t source = 0;
  std::uint8_t aux = aux_none;
  bool bipolar = false;
  bool targets_amount = false; // Its destination is another route's amount.
  float tension = 0.0F;
  float scale = 1.0F;
};

// Modulated plain values for one consumer (a voice, or the global path):
// the base values plus every routed destination's offset, sparse. Sized at
// construction for the table; reads are O(1).
class Modulated {
public:
  explicit Modulated(std::size_t parameters)
      : value_(parameters, 0.0), touched_(parameters, 0), contribution_(route_slots, 0.0) {
    list_.reserve(parameters);
  }
  // Evaluates every route against base (plain, table order) and sources.
  // Routes that target amounts run first, so an amount's modulation is heard
  // the same tick. Contributions are in normalised destination units.
  void evaluate(std::span<const double> base, std::span<const CompiledRoute> routes,
                const SourceValues& sources) noexcept;
  [[nodiscard]] double get(std::size_t index) const noexcept {
    return touched_[index] ? value_[index] : base_[index];
  }
  [[nodiscard]] double contribution(std::size_t slot) const noexcept { return contribution_[slot]; }
  [[nodiscard]] std::span<const double> base() const noexcept { return base_; }

private:
  std::span<const double> base_{};
  std::vector<double> value_; // Normalised sums while evaluating, then plain.
  std::vector<std::uint8_t> touched_;
  std::vector<std::uint16_t> list_;
  std::vector<double> contribution_; // Per route slot, normalised.
};

// AHDSR with bent segments, loop and legato (08a §2.5). Per sample.
struct EnvelopeSettings {
  double attack = 0.01, hold = 0.0, decay = 0.5, sustain = 0.4, release = 0.4;
  double attack_curve = 0.0, decay_curve = 0.4, release_curve = 0.4;
  bool loop = false, legato = false;
};
class Envelope {
public:
  enum class Stage : std::uint8_t { idle, attack, hold, decay, sustain, release };
  void prepare(double sample_rate) noexcept { rate_ = sample_rate; }
  void set(const EnvelopeSettings& settings) noexcept { settings_ = settings; }
  // A note starts: from the current level (no click on a retrigger), or,
  // with legato and a sounding envelope, it carries on.
  void gate_on(bool legato_note) noexcept;
  void gate_off() noexcept;
  void reset() noexcept {
    stage_ = Stage::idle;
    level_ = 0.0;
    progress_ = 0.0;
  }
  // Advances `frames` samples and returns the level after them.
  double advance(std::uint32_t frames) noexcept;
  [[nodiscard]] double next() noexcept { return advance(1); }
  [[nodiscard]] double level() const noexcept { return level_; }
  [[nodiscard]] Stage stage() const noexcept { return stage_; }
  [[nodiscard]] double progress() const noexcept { return progress_; }
  [[nodiscard]] bool active() const noexcept { return stage_ != Stage::idle; }

private:
  [[nodiscard]] double seconds(Stage stage) const noexcept;
  void enter(Stage stage) noexcept;
  EnvelopeSettings settings_{};
  double rate_ = 48'000.0;
  Stage stage_ = Stage::idle;
  double level_ = 0.0;
  double start_ = 0.0; // Level at the segment start.
  double progress_ = 0.0;
};

// A drawn LFO (08a §2.6). Per control tick.
struct LfoSettings {
  double rate_hz = 2.0;
  std::size_t sync = 9; // Index into lfo_divisions; 0 is Hz.
  LfoMode mode = LfoMode::trig;
  double rise = 0.0, delay = 0.0, smooth = 0.0, phase = 0.0; // phase in cycles
};
class Lfo {
public:
  void reset() noexcept;
  // A note starts (Trig and Env restart; Free never does).
  void trigger() noexcept;
  // Advances one tick of `seconds` and returns s (0..1). `song_cycles` is the
  // free-running phase the transport implies for this rate.
  double tick(const LfoSettings& settings, const CurveShape& shape, double seconds,
              double song_cycles) noexcept;
  [[nodiscard]] double value() const noexcept { return value_; }
  [[nodiscard]] double phase() const noexcept { return phase_; }
  // The rise gain for routes (0..1).
  [[nodiscard]] double gain() const noexcept { return gain_; }

private:
  double phase_ = 0.0;   // Running phase for Trig/Env, cycles.
  double elapsed_ = 0.0; // Seconds since the trigger.
  double value_ = 0.0;
  double smoothed_ = 0.0;
  double gain_ = 1.0;
  bool started_ = false;
  bool finished_ = false; // Env mode ran its one cycle.
};

// Two chaotic sources (08a §2.7): Lorenz and the logistic map. Seeded, so a
// render is deterministic.
class Chaos {
public:
  void reset() noexcept;
  // Advances one tick and returns s (0..1).
  double tick(std::size_t which, double rate_hz, double smooth, double seconds) noexcept;

private:
  double x_ = 0.1, y_ = 0.0, z_ = 0.0; // Lorenz.
  double logistic_ = 0.3141;
  double logistic_phase_ = 0.0;
  std::array<double, 2> raw_{};
  std::array<double, 2> smoothed_{};
};

// A deterministic per-note value in 0..1 from a seed (note id, voice).
[[nodiscard]] inline double hash_unit(std::uint64_t seed) noexcept {
  seed ^= seed >> 33U;
  seed *= 0xff51afd7ed558ccdULL;
  seed ^= seed >> 33U;
  seed *= 0xc4ceb9fe1a85ec53ULL;
  seed ^= seed >> 33U;
  return static_cast<double>(seed >> 11U) * (1.0 / 9007199254740992.0);
}

} // namespace oscillate::native_dsp::synth::nova
