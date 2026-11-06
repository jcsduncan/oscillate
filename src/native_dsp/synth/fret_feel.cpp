#include "oscillate/native_dsp/synth/fret_feel.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::synth::fret::feel {
namespace {

constexpr double sixteenths_per_beat = 4.0;
constexpr double sixteenth_beats = 1.0 / sixteenths_per_beat;
constexpr double eighth_beats = 2.0 * sixteenth_beats;
constexpr int sixteenths_per_eighth = 2;
// A note within this many beats of a grid point is on it (1/48 beat).
constexpr double grid_tolerance_beats = 1.0 / 48.0;
// Two notes this close to a sixteenth apart are a run.
constexpr double run_tolerance_beats = 0.5 * sixteenth_beats;
constexpr int octave = 12;

// Each place's weight is centred on this, and a step of this much (one
// place stronger) moves the velocity by a whole Dynamics.
constexpr double weight_centre = 0.75;
constexpr double weight_step = 0.25;
// Shares of Dynamics each gesture adds.
constexpr double run_share = 0.15;
constexpr int run_notes = 4;
constexpr double rest_share = 0.25;
constexpr double rest_beats = 0.5;
constexpr double pop_share = 0.35;
constexpr double push_share = 1.0;
// A note this many sixteenths long or shorter is short.
constexpr double short_sixteenths = 1.0;
// Humanize repeats every this many bars, so a loop keeps its groove.
constexpr std::int64_t humanize_cycle_bars = 2;

struct Weights {
  double one;
  double beat;
  double eighth;
  double sixteenth;
};
// Per style, in Style order; As drawn has none.
constexpr std::array<Weights, 4> style_weights{{
    {0.0, 0.0, 0.0, 0.0},   // As drawn.
    {1.0, 0.8, 0.7, 0.5},   // Funk: leans on the one.
    {1.0, 0.9, 0.85, 0.6},  // Driving: every eighth up.
    {0.9, 0.8, 0.75, 0.65}, // Laid back: flatter.
}};

[[nodiscard]] double weight_of(const Weights& w, Place place) noexcept {
  switch (place) {
  case Place::one:
    return w.one;
  case Place::beat:
    return w.beat;
  case Place::eighth:
    return w.eighth;
  case Place::sixteenth:
  case Place::between:
    break;
  }
  return w.sixteenth;
}

[[nodiscard]] bool off_beat(Place place) noexcept {
  return place == Place::eighth || place == Place::sixteenth || place == Place::between;
}

// A seeded -1..1 from the note's sixteenth in its humanize cycle and its key
// (SplitMix64's finaliser).
[[nodiscard]] double jitter(const Note& note) noexcept {
  const auto cycle_bar =
      ((note.bar % humanize_cycle_bars) + humanize_cycle_bars) % humanize_cycle_bars;
  const auto sixteenth =
      static_cast<std::int64_t>(std::llround(note.bar_beat * sixteenths_per_beat));
  std::uint64_t x = (static_cast<std::uint64_t>(cycle_bar) << 32U) ^
                    (static_cast<std::uint64_t>(sixteenth) << 8U) ^
                    static_cast<std::uint64_t>(note.key);
  x = native_dsp::dsp::splitmix64(x);
  constexpr double unit = 1.0 / 18'446'744'073'709'551'616.0; // 2^-64.
  return static_cast<double>(x) * unit * 2.0 - 1.0;
}

} // namespace

Place place_of(double bar_beat) noexcept {
  const double sixteenths = bar_beat * sixteenths_per_beat;
  const auto nearest = std::llround(sixteenths);
  if (std::fabs(sixteenths - static_cast<double>(nearest)) * sixteenth_beats >
      grid_tolerance_beats) {
    return Place::between;
  }
  if (nearest == 0) {
    return Place::one;
  }
  const auto per_beat = static_cast<long long>(sixteenths_per_beat);
  if (nearest % per_beat == 0) {
    return Place::beat;
  }
  return nearest % sixteenths_per_eighth == 0 ? Place::eighth : Place::sixteenth;
}

Decision decide(const Note& note, const Previous& previous, const Settings& settings) noexcept {
  Decision d;
  d.place = place_of(note.bar_beat);
  d.velocity = std::clamp(note.velocity, 1, max_velocity);
  const double amount = std::clamp(settings.amount, 0.0, 1.0);
  if (settings.style == Style::as_drawn || amount <= 0.0) {
    return d;
  }
  const auto& weights = style_weights[static_cast<std::size_t>(settings.style)];
  const double dynamics = std::clamp(settings.dynamics, 0.0, max_dynamics);
  double v =
      centre_velocity + dynamics * (weight_of(weights, d.place) - weight_centre) / weight_step;

  const double gap =
      previous.valid ? note.beat - (previous.beat + previous.length_beats) : rest_beats;
  const bool in_run = previous.valid &&
                      std::fabs(note.beat - previous.beat - sixteenth_beats) < run_tolerance_beats;
  d.run = in_run ? previous.run + 1 : 0;
  v += static_cast<double>(std::min(d.run, run_notes)) * dynamics * run_share;
  if (gap >= rest_beats) {
    v += dynamics * rest_share;
  }
  const bool popped = previous.valid && note.key - previous.key >= octave;
  if (popped) {
    v += dynamics * pop_share;
  }
  const bool weak = off_beat(d.place);
  const bool known = note.length_beats > 0.0;
  if (weak && known) {
    const double next_eighth =
        (std::floor(note.bar_beat / eighth_beats + grid_tolerance_beats) + 1.0) * eighth_beats;
    if (note.bar_beat + note.length_beats > next_eighth) {
      v += dynamics * push_share;
      d.pushed = true;
    }
  }
  const bool short_note = known && note.length_beats <= short_sixteenths * sixteenth_beats;
  const bool repeat = previous.valid && note.key == previous.key && !popped;
  auto articulation = Articulation::pluck;
  const bool live_ghost = !known && note.velocity <= live_ghost_velocity;
  if (settings.ghosts && weak && repeat && (short_note || live_ghost)) {
    articulation = Articulation::ghost;
    v = ghost_velocity;
  } else if (short_note && weak) {
    articulation = Articulation::staccato;
  }
  v += std::clamp(settings.humanize, 0.0, max_humanize) * jitter(note);
  const double own = static_cast<double>(d.velocity);
  d.velocity = std::clamp(static_cast<int>(std::lround(own + (v - own) * amount)), 1, max_velocity);
  d.articulation = amount < articulation_amount ? Articulation::pluck : articulation;
  if (weak) {
    d.delay_ms = std::clamp(settings.pocket_ms, 0.0, max_pocket_ms) * amount;
  }
  return d;
}

Previous after(const Note& note, const Decision& decision) noexcept {
  return {true, note.beat, note.length_beats, note.key, decision.run};
}

} // namespace oscillate::native_dsp::synth::fret::feel
