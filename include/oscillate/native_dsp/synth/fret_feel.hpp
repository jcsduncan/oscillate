#pragma once

#include <cstdint>

// Fret's Feel (ADR-0088): realtime auto-articulation. When a note starts,
// Feel decides its velocity, its articulation and how late it plays from
// what is known then: its place in the bar, its own length (clip notes
// carry it), its key and velocity, and the note before it. Never a later
// note, so clips and live playing behave alike. A pure function: the same
// inputs give the same decision, so export equals playback.
namespace oscillate::native_dsp::synth::fret::feel {

enum class Style : std::uint8_t { as_drawn, funk, driving, laid_back };
enum class Articulation : std::uint8_t { pluck, staccato, ghost, bridge };
// Where a note falls: on the bar's first beat, another beat, an off-beat
// eighth or sixteenth, or off the sixteenth grid.
enum class Place : std::uint8_t { one, beat, eighth, sixteenth, between };

inline constexpr int max_velocity = 127;
// An average note's velocity, before Dynamics moves it.
inline constexpr double centre_velocity = 92.0;
// The velocity Feel gives a ghost note.
inline constexpr int ghost_velocity = 44;
// A live note (length unknown) this soft on an off-beat repeat is a ghost.
inline constexpr int live_ghost_velocity = 50;
// Below this Amount every note keeps the pluck articulation.
inline constexpr double articulation_amount = 0.5;
// Ranges of the controls.
inline constexpr double max_dynamics = 30.0;
inline constexpr double max_pocket_ms = 20.0;
inline constexpr double max_humanize = 10.0;

struct Settings {
  Style style = Style::funk;
  double amount = 1.0;    // 0 plays the notes' own velocities, 1 Feel's.
  double dynamics = 20.0; // How far accents reach, in velocity steps.
  bool ghosts = true;
  double humanize = 0.0;  // Seeded velocity variation, ± steps.
  double pocket_ms = 0.0; // How late off-beats sit.
};

struct Note {
  double beat = 0.0;         // Quarter notes from the song start.
  double bar_beat = 0.0;     // Quarter notes from its bar's start.
  std::int64_t bar = 0;      // Its bar, zero-based.
  double length_beats = 0.0; // 0 when unknown (a live note).
  int key = 0;
  int velocity = 0;
};

// The note before, on the same instrument. `valid` is false for the first
// note after a reset.
struct Previous {
  bool valid = false;
  double beat = 0.0;
  double length_beats = 0.0;
  int key = 0;
  int run = 0; // Notes in the sixteenth run it ended.
};

struct Decision {
  int velocity = 0;
  Articulation articulation = Articulation::pluck;
  bool pushed = false; // An off-beat held past the next eighth.
  double delay_ms = 0.0;
  Place place = Place::one;
  int run = 0;
};

[[nodiscard]] Place place_of(double bar_beat) noexcept;

[[nodiscard]] Decision decide(const Note& note, const Previous& previous,
                              const Settings& settings) noexcept;

// The Previous the next note sees after `note` played as `decision`.
[[nodiscard]] Previous after(const Note& note, const Decision& decision) noexcept;

} // namespace oscillate::native_dsp::synth::fret::feel
