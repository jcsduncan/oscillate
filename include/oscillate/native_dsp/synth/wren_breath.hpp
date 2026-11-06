#pragma once

#include "oscillate/project/model.hpp"

#include <cstdint>
#include <optional>

// Wren's Breath (ADR-0089): Feel for a flute. When a note starts, Breath
// decides how a flautist plays it from what is known then: its place in the
// bar and the tempo, its own length (clip notes carry it; 0 for live notes),
// its key and velocity, the note before it on this kernel and the project
// key. Never a later note, so clips and live playing behave alike. A pure
// function: the same inputs give the same decision, so export equals
// playback.
namespace oscillate::native_dsp::synth::wren::breath {

enum class Style : std::uint8_t { as_drawn, lyrical, airy };
enum class Ornaments : std::uint8_t { off, some, lots };
// How notes join (the Articulation control).
enum class Articulation : std::uint8_t { automatic, legato, tongued, staccato };
// How a note starts: tongued with the front of the tongue (t) or the back
// (k, softer), or slurred from the note before (no new attack).
enum class Tongue : std::uint8_t { t, k, slur };
// Where a note falls: on the bar's first beat, another beat, an off-beat
// eighth, or anywhere else (a sixteenth or off the grid).
enum class Place : std::uint8_t { one, beat, eighth, sixteenth };

inline constexpr int max_velocity = 127;
// Breath never plays a note softer than this.
inline constexpr int min_velocity = 20;
// Ranges of the controls.
inline constexpr double max_dynamics = 30.0;
inline constexpr double max_swell_db = 6.0;
inline constexpr double min_bloom_ms = 80.0;
inline constexpr double max_bloom_ms = 500.0;
inline constexpr double max_humanize = 10.0;
// Humanize detunes each note by up to this many cents at full Humanize.
inline constexpr double max_humanize_cents = 3.0;
// The defaults: the Lyrical style as the prototype played it. Airy scales
// Dynamics, Swell and Bloom by its own factors (wren_breath.cpp).
inline constexpr double default_dynamics = 16.0;
inline constexpr double default_swell_db = 3.0;
inline constexpr double default_bloom_ms = 400.0;
// A k is this much softer than a t, and fades in over its first frames.
inline constexpr double k_tongue_db = -2.0;
inline constexpr double k_onset_ms = 8.0;
// Notes under this many beats are staccato under Auto (and every note is
// held at most this long under Staccato).
inline constexpr double staccato_beats = 1.0 / 3.0;

struct Settings {
  Style style = Style::lyrical;
  double amount = 1.0;                // 0 plays the notes' own velocities, 1 Breath's.
  double dynamics = default_dynamics; // How far accents reach, in velocity steps.
  double swell_db = default_swell_db;
  double bloom_ms = default_bloom_ms; // How long vibrato takes to bloom.
  Ornaments ornaments = Ornaments::some;
  double humanize = 0.0; // Seeded variation: ± velocity steps (and cents).
  Articulation articulation = Articulation::automatic;
};

// The project key's scale as twelve pitch classes from the tonic, bit n set
// when the note n semitones above the tonic is in the scale.
struct Scale {
  int tonic = 0;
  std::uint16_t pitch_classes = 0;
};

// The scale of the project's key (major or natural minor); nullopt without
// one.
[[nodiscard]] std::optional<Scale> scale_of(std::optional<project::MusicalKey> key) noexcept;

struct Context {
  double beat = 0.0;          // Quarter notes from the song start.
  double bar_beat = 0.0;      // Quarter notes from its bar's start.
  std::int64_t bar = 0;       // Its bar, zero-based.
  double beats_per_bar = 4.0; // The bar's length in quarter notes.
  double length_beats = 0.0;  // 0 when unknown (a live note).
  int key = 0;
  int velocity = 0;
  std::optional<Scale> scale; // The project key's; nullopt without one.
};

// The note before, on the same kernel. `valid` is false for the first note
// after a reset. A clip note's end is known when it starts; a live note's
// once its note-off arrives (`ended` false until then).
struct Previous {
  bool valid = false;
  double beat = 0.0;
  double length_beats = 0.0; // 0 when unknown.
  bool ended = false;
  double end_beat = 0.0; // Where it ends or ended, when `ended`.
  int key = 0;
  Tongue tongue = Tongue::t;
};

struct Decision {
  int velocity = 0;
  Tongue tongue = Tongue::t;
  bool staccato = false; // Short and light: the short release.
  // Vibrato: the share of the vibrato take at full bloom (0 for none,
  // before the Vibrato control scales it), the delay before it blooms and
  // how long the bloom takes.
  double vibrato = 0.0;
  double vibrato_delay_ms = 0.0;
  double bloom_ms = 0.0;
  double swell_db = 0.0; // How far the note leans in; 0 for none.
  // A grace note: `grace_step` semitones above, played on the beat for
  // `grace_ms`, then the note slurred from it.
  bool grace = false;
  int grace_step = 0;
  double grace_ms = 0.0;
  double cents = 0.0; // Humanize's detune.
  Place place = Place::one;
};

[[nodiscard]] Place place_of(double bar_beat, double beats_per_bar) noexcept;

// The semitones from `key` up to the next note of `scale` (1 or 2), a whole
// tone without a scale.
[[nodiscard]] int step_above(int key, std::optional<Scale> scale) noexcept;

[[nodiscard]] Decision decide(const Context& note, const Previous& previous,
                              const Settings& settings) noexcept;

// The Previous the next note sees after `note` started as `decision`.
[[nodiscard]] Previous after(const Context& note, const Decision& decision) noexcept;

} // namespace oscillate::native_dsp::synth::wren::breath
