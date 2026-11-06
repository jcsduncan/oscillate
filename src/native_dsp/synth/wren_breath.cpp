#include "oscillate/native_dsp/synth/wren_breath.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace oscillate::native_dsp::synth::wren::breath {
namespace {

constexpr int semitones_per_octave = 12;
constexpr double sixteenth_beats = 0.25;
constexpr double eighth_beats = 0.5;
constexpr double beat_beats = 1.0;
// A note within this many beats of a grid point is on it (1/48 beat).
constexpr double grid_tolerance_beats = 1.0 / 48.0;
// Ends this close count as touching, not overlapping.
constexpr double overlap_epsilon_beats = 1e-6;
// Each place's weight is centred on this, and a step of this much moves the
// velocity by a whole Dynamics.
constexpr double weight_centre = 0.8;
constexpr double weight_step = 0.2;
// A note of a beat or more is long: it plays louder and may take a grace.
constexpr double long_beats = 1.0;
// This much silence before a note is a rest: the next phrase starts louder.
constexpr double rest_beats = 0.5;
// A flute's upper register speaks louder: above D5 each semitone adds this
// much velocity, and below it each takes it away.
constexpr int register_centre_key = 74; // D5.
constexpr double register_velocity_per_semitone = 0.5;
// Repeated notes this close (a sixteenth, with a hair of slack) are fast:
// the second is tongued k.
constexpr double fast_repeat_beats = 0.26;
// Legato also slurs across a gap of up to a sixteenth.
constexpr double legato_gap_beats = sixteenth_beats;
// Seeds: the place in a two-bar cycle at 1/48 of a beat, the key, and a salt
// per decision.
constexpr std::int64_t seed_cycle_bars = 2;
constexpr double seed_steps_per_beat = 48.0;
constexpr std::uint64_t grace_salt = 1;
constexpr std::uint64_t velocity_salt = 2;
constexpr std::uint64_t cents_salt = 3;
// Ornaments: the chance a note that may take a grace gets one.
constexpr double some_grace_chance = 0.6;
constexpr double lots_grace_chance = 0.75;
// Major and natural minor, as pitch classes above the tonic.
constexpr std::array major_steps{0, 2, 4, 5, 7, 9, 11};
constexpr std::array minor_steps{0, 2, 3, 5, 7, 8, 10};
constexpr int whole_tone = 2;

// One style's playing.
struct Shape {
  std::array<double, 4> weights; // The one, a beat, an eighth, anything else.
  double centre;                 // An average note's velocity.
  double long_share;             // Shares of Dynamics a long note and a rest add.
  double rest_share;
  double vibrato_beats;    // Notes this long or longer get vibrato...
  double vibrato_delay_ms; // ...after this...
  double vibrato_depth;    // ...up to this share of the vibrato take.
  double swell_beats;      // Notes this long or longer swell.
  int grace_leap;          // A grace needs a leap of at least this many semitones.
  double grace_ms;
  // The style's Dynamics, Swell and Bloom relative to the controls (which
  // are Lyrical's).
  double dynamics_scale;
  double swell_scale;
  double bloom_scale;
};

// Airy plays Dynamics 14, a 1.5 dB swell and a 600 ms bloom where Lyrical
// plays 16, 3 dB and 400 ms.
constexpr double airy_dynamics = 14.0;
constexpr double airy_swell_db = 1.5;
constexpr double airy_bloom_ms = 600.0;

constexpr Shape lyrical{
    {1.0, 0.9, 0.82, 0.76}, 84.0, 0.35, 0.2, 0.75, 160.0, 1.0, 1.5, 3, 70.0, 1.0, 1.0, 1.0};
constexpr Shape airy{{1.0, 0.85, 0.75, 0.7},
                     78.0,
                     0.2,
                     0.25,
                     1.5,
                     350.0,
                     0.55,
                     2.0,
                     0,
                     35.0,
                     airy_dynamics / default_dynamics,
                     airy_swell_db / default_swell_db,
                     airy_bloom_ms / default_bloom_ms};

[[nodiscard]] const Shape& shape_of(Style style) noexcept {
  return style == Style::airy ? airy : lyrical;
}

// A fraction in [0, 1) from the note's place in a two-bar cycle, its key
// and a salt (SplitMix64's finaliser).
[[nodiscard]] double seeded(const Context& note, std::uint64_t salt) noexcept {
  // The key is spread by SplitMix64's first finaliser multiplier.
  constexpr std::uint64_t key_spread = 0xBF58'476D'1CE4'E5B9ULL;
  const auto cycle_bar = ((note.bar % seed_cycle_bars) + seed_cycle_bars) % seed_cycle_bars;
  const double cycle_beat = static_cast<double>(cycle_bar) * note.beats_per_bar + note.bar_beat;
  const auto step = static_cast<std::uint64_t>(std::llrint(cycle_beat * seed_steps_per_beat));
  const std::uint64_t x = step * native_dsp::dsp::golden_gamma +
                          static_cast<std::uint64_t>(note.key) * key_spread + salt;
  return native_dsp::dsp::unit_from_bits(native_dsp::dsp::splitmix64_finalise(x));
}

// A seeded -1..1.
[[nodiscard]] double jitter(const Context& note, std::uint64_t salt) noexcept {
  return 2.0 * seeded(note, salt) - 1.0;
}

[[nodiscard]] double weight_of(const Shape& shape, Place place) noexcept {
  return shape.weights[static_cast<std::size_t>(place)];
}

[[nodiscard]] double grace_chance(Ornaments ornaments) noexcept {
  switch (ornaments) {
  case Ornaments::some:
    return some_grace_chance;
  case Ornaments::lots:
    return lots_grace_chance;
  case Ornaments::off:
    break;
  }
  return 0.0;
}

[[nodiscard]] bool on_grid(double beat, double step) noexcept {
  return std::fabs(beat / step - std::nearbyint(beat / step)) * step < grid_tolerance_beats;
}

} // namespace

std::optional<Scale> scale_of(std::optional<project::MusicalKey> key) noexcept {
  if (!key) {
    return std::nullopt;
  }
  Scale scale;
  scale.tonic = key->tonic % semitones_per_octave;
  const auto set = [&](const auto& steps) {
    for (const int step : steps) {
      scale.pitch_classes = static_cast<std::uint16_t>(scale.pitch_classes | (1U << step));
    }
  };
  if (key->mode == project::KeyMode::minor) {
    set(minor_steps);
  } else {
    set(major_steps);
  }
  return scale;
}

Place place_of(double bar_beat, double beats_per_bar) noexcept {
  if (on_grid(bar_beat, beats_per_bar)) {
    return Place::one;
  }
  if (on_grid(bar_beat, beat_beats)) {
    return Place::beat;
  }
  if (on_grid(bar_beat, eighth_beats)) {
    return Place::eighth;
  }
  return Place::sixteenth;
}

int step_above(int key, std::optional<Scale> scale) noexcept {
  if (!scale) {
    return whole_tone;
  }
  for (int up = 1; up <= whole_tone; ++up) {
    const int pitch_class =
        ((key + up - scale->tonic) % semitones_per_octave + semitones_per_octave) %
        semitones_per_octave;
    if ((scale->pitch_classes & (1U << static_cast<unsigned>(pitch_class))) != 0) {
      return up;
    }
  }
  return whole_tone;
}

Decision decide(const Context& note, const Previous& previous, const Settings& settings) noexcept {
  Decision d;
  d.place = place_of(note.bar_beat, note.beats_per_bar);
  d.velocity = std::clamp(note.velocity, 1, max_velocity);
  const bool known = note.length_beats > 0.0;

  // How the note joins the one before.
  const bool overlap =
      previous.valid && (!previous.ended || previous.end_beat > note.beat + overlap_epsilon_beats);
  const double gap = previous.valid && previous.ended ? note.beat - previous.end_beat : 0.0;
  const bool short_note = known && note.length_beats < staccato_beats;
  bool slur = false;
  switch (settings.articulation) {
  case Articulation::automatic:
    slur = overlap;
    d.staccato = short_note;
    break;
  case Articulation::legato:
    slur = overlap || (previous.valid && gap <= legato_gap_beats + overlap_epsilon_beats);
    d.staccato = short_note;
    break;
  case Articulation::tongued:
    d.staccato = short_note;
    break;
  case Articulation::staccato:
    d.staccato = true;
    break;
  }
  d.tongue = slur ? Tongue::slur : Tongue::t;

  const double amount = std::clamp(settings.amount, 0.0, 1.0);
  if (settings.style == Style::as_drawn || amount <= 0.0) {
    return d;
  }
  const auto& shape = shape_of(settings.style);
  const double dynamics = std::clamp(settings.dynamics, 0.0, max_dynamics) * shape.dynamics_scale;

  // Velocity: the place's weight, long notes, phrase starts and register.
  double v = shape.centre + dynamics * (weight_of(shape, d.place) - weight_centre) / weight_step;
  if (known && note.length_beats >= long_beats) {
    v += dynamics * shape.long_share;
  }
  const bool rest = !previous.valid || (previous.ended && gap >= rest_beats);
  if (rest) {
    v += dynamics * shape.rest_share;
  }
  v += (note.key - register_centre_key) * register_velocity_per_semitone;
  const double humanize = std::clamp(settings.humanize, 0.0, max_humanize);
  v += humanize * jitter(note, velocity_salt);
  const double target =
      std::clamp(v, static_cast<double>(min_velocity), static_cast<double>(max_velocity));
  const double own = static_cast<double>(d.velocity);
  // Round half to even.
  d.velocity =
      std::clamp(static_cast<int>(std::nearbyint(own + (target - own) * amount)), 1, max_velocity);
  d.cents = max_humanize_cents * humanize / max_humanize * jitter(note, cents_salt) * amount;

  // Vibrato blooms on long notes, and on every live note held long enough.
  if (!known || note.length_beats >= shape.vibrato_beats) {
    d.vibrato = shape.vibrato_depth * amount;
    d.vibrato_delay_ms = shape.vibrato_delay_ms;
    d.bloom_ms = std::clamp(settings.bloom_ms, min_bloom_ms, max_bloom_ms) * shape.bloom_scale;
  }
  // Long clip notes lean in; a live note's end is unknown.
  if (known && note.length_beats >= shape.swell_beats) {
    d.swell_db = std::clamp(settings.swell_db, 0.0, max_swell_db) * shape.swell_scale * amount;
  }
  // A grace from the scale step above, on long notes on the beat after a
  // leap.
  const bool on_beat = d.place == Place::one || d.place == Place::beat;
  const bool leap = previous.valid && std::abs(note.key - previous.key) >= shape.grace_leap;
  if (known && note.length_beats >= long_beats && on_beat && leap &&
      seeded(note, grace_salt) < grace_chance(settings.ornaments) * amount) {
    d.grace = true;
    d.grace_step = step_above(note.key, note.scale);
    d.grace_ms = shape.grace_ms;
  }
  // Fast repeats double-tongue: k after a t.
  const bool fast = !known || note.length_beats <= fast_repeat_beats;
  if (!slur && previous.valid && previous.key == note.key && fast &&
      note.beat - previous.beat <= fast_repeat_beats + overlap_epsilon_beats &&
      previous.tongue == Tongue::t) {
    d.tongue = Tongue::k;
  }
  return d;
}

Previous after(const Context& note, const Decision& decision) noexcept {
  Previous p;
  p.valid = true;
  p.beat = note.beat;
  p.length_beats = note.length_beats;
  p.ended = note.length_beats > 0.0;
  p.end_beat = note.beat + note.length_beats;
  p.key = note.key;
  p.tongue = decision.tongue;
  return p;
}

} // namespace oscillate::native_dsp::synth::wren::breath
