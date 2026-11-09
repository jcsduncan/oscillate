#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace oscillate::native_dsp::synth::bow {

// Bow (ADR-0111): sampled strings, sections and solo. Sections are VSCO 2
// Community Edition (CC0): violins, violas and cellos as recorded sections,
// the basses as the solo contrabass layered. Solo is the University of Iowa
// MIS violin, viola, cello and double bass. Installed by
// tools/bow/install_strings.py and held in memory whole.
//
// The mod wheel (bow.perf.wheel) bows the dynamics in realtime, crossfading
// the recorded layers; Legato joins overlapping notes past the next note's
// attack; keyswitches below the range latch the articulation; Tone moves
// from sul tasto through ordinario to sul ponticello; Ensemble layers each
// note into detuned, delayed players; Humanise spreads every player's tuning
// and timing. Without the library Bow is silent and says so. Defaults are
// the "Sul Tasto Pad" preset.
[[nodiscard]] const SynthEngine& engine();

// The panel's choices, in table order.
enum class Section : std::uint8_t { violins_1, violins_2, violas, cellos, basses, ensemble };
enum class Mode : std::uint8_t { solo, section };
enum class Articulation : std::uint8_t { sustain, legato, staccato, pizzicato, tremolo };
inline constexpr std::size_t section_count = 6;
inline constexpr std::size_t articulation_count = 5;

// The recorded instruments a section plays.
enum class Instrument : std::uint8_t { violin, viola, cello, bass };
inline constexpr std::size_t instrument_count = 4;

// The sounding keys, E1 to G7; each instrument plays its own range within.
inline constexpr int first_key = 28;
inline constexpr int last_key = 103;
inline constexpr std::size_t key_count = last_key - first_key + 1;
struct KeyRange {
  int low = 0;
  int high = 0;
};
// Violin G3-G7, viola C3-E6, cello C2-E5, bass E1-G4 (in Instrument order).
inline constexpr std::array<KeyRange, instrument_count> instrument_ranges{{
    {55, 103},
    {48, 88},
    {36, 76},
    {28, 67},
}};
// What a section plays across the keyboard: its instrument's range, or
// Ensemble's whole span.
[[nodiscard]] KeyRange playable(Section section) noexcept;
// The instrument a section plays `key` on (Ensemble: by register).
[[nodiscard]] Instrument instrument_for(Section section, int key) noexcept;

// Keyswitches, below every range: C0 Sustain, C#0 Legato, D0 Staccato,
// D#0 Pizzicato, E0 Tremolo. A keyswitch latches its articulation until the
// panel's changes or a reset.
inline constexpr int first_keyswitch = 12;
inline constexpr int last_keyswitch = first_keyswitch + static_cast<int>(articulation_count) - 1;
[[nodiscard]] constexpr bool is_keyswitch(int key) noexcept {
  return key >= first_keyswitch && key <= last_keyswitch;
}

// Up to this many recorded dynamics per note (Iowa: pp, mf, ff; VSCO: two).
inline constexpr std::uint32_t max_layers = 3;
// Up to this many round robins per note and layer.
inline constexpr std::uint32_t max_round_robins = 4;
// Ensemble: every players_per_layer players add a layer, up to max_players.
inline constexpr int min_players = 1;
inline constexpr int max_players = 16;
inline constexpr int players_per_layer = 4;
inline constexpr std::size_t max_ensemble_layers = max_players / players_per_layer;
// The layers a note plays for an Ensemble setting.
[[nodiscard]] std::size_t ensemble_layers(double players) noexcept;

// The dynamics the mod wheel reads as, from ppp to ff, for the panel: each
// name up to its top (the wheel's 0..1), ff above the last.
inline constexpr std::array<std::string_view, 7> dynamic_names{"ppp", "pp", "p", "mp",
                                                               "mf",  "f",  "ff"};
inline constexpr std::array<double, dynamic_names.size() - 1> dynamic_tops{0.1, 0.22, 0.34,
                                                                           0.5, 0.66, 0.82};
[[nodiscard]] std::string_view dynamic_name(double wheel) noexcept;

// Telemetry layout (ADR-0037).
namespace telemetry {
inline constexpr std::size_t library = 0;        // library::State.
inline constexpr std::size_t notes = 1;          // Notes started since reset (wraps at 2^24).
inline constexpr std::size_t articulation = 2;   // The articulation playing (Articulation).
inline constexpr std::size_t latched = 3;        // 1 when a keyswitch chose it.
inline constexpr std::size_t dynamics = 4;       // The wheel as played, 0..1.
inline constexpr std::size_t vibrato = 5;        // The newest note's vibrato now, 0..1.
inline constexpr std::size_t voices = 6;         // Voices sounding (players of every note).
inline constexpr std::size_t output_peak_db = 7; // dBFS this block, >= -120.
inline constexpr std::size_t held_count = 8;     // Keys held, up to held_slots of them below.
inline constexpr std::size_t held = 9;           // Held keys, -1 for an empty slot.
inline constexpr std::size_t held_slots = 16;
inline constexpr std::size_t size = held + held_slots;
} // namespace telemetry

// The sample library: one per process, shared by every Bow. All functions
// are control-thread only.
namespace library {

inline constexpr std::string_view name =
    "VSCO 2 CE strings and University of Iowa MIS solo strings";
inline constexpr std::string_view attribution =
    "VSCO 2 Community Edition by Versilian Studios (Sam Gossner), CC0 1.0; "
    "University of Iowa Musical Instrument Samples (Lawrence Fritts)";
inline constexpr std::string_view install_command = "python3 -I tools/bow/install_strings.py";
// Overrides the directory (tests, or a library kept elsewhere).
inline constexpr std::string_view directory_variable = "OSCILLATE_BOW_LIBRARY";
inline constexpr std::string_view manifest_file = "bow.manifest";
inline constexpr std::string_view format_line = "oscillate bow library 1";
// Both sources' rate; Bow resamples to the session's.
inline constexpr std::uint32_t pack_rate = 44'100;

// `loading` while the first look reads the library in the background.
enum class State : std::uint8_t { missing, invalid, ready, loading };

struct Status {
  State state = State::missing;
  std::string message; // Why it is missing or invalid; empty when ready.
  std::filesystem::path directory;
  std::string name;
  std::string attribution;
  std::size_t samples = 0;
  std::uint64_t resident_bytes = 0;
};

// $OSCILLATE_BOW_LIBRARY when set, otherwise
// $XDG_DATA_HOME/oscillate/instruments/bow/strings (~/.local/share when
// XDG_DATA_HOME is unset), as the install script uses.
[[nodiscard]] std::filesystem::path default_directory();

// The library's state. The first look while none is loaded starts reading
// the default directory in the background and reports `loading` until it
// is done; while missing or invalid each later call looks again when the
// directory or its manifest changed.
[[nodiscard]] Status status();

// Loads the library in `directory` now and makes it the one new and idle
// Bows play. A loaded library stays in memory until the process exits.
[[nodiscard]] Status load(const std::filesystem::path& directory);

// Waits for a background load status() started (offline renders, before
// their first block).
void wait();

} // namespace library

} // namespace oscillate::native_dsp::synth::bow
