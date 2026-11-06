#pragma once

#include "oscillate/application/browser_preview.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/timeline/timing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Data tables keep one entry per row; clang-format would split them.
// clang-format off

// Knock's patterns (ADR-0068, docs/ui/knock-mockup.html): loops of eight
// lanes (one per pad) of sixteen sixteenths a bar, a generator that writes
// each pad the kind of part its sound suits (Roll, Mutate, lane locks and
// Evolve), a library of named loops, and the notes a pattern writes into a
// MIDI clip. Pure and deterministic: every random choice comes from a
// seeded generator, so the same seed is the same loop everywhere. No Qt, no
// I/O, no project edits.
namespace oscillate::application::knock_patterns {

namespace knock = native_dsp::synth::knock;

inline constexpr std::size_t lane_count = knock::pad_count;
inline constexpr std::size_t steps_per_bar = 16;
inline constexpr std::size_t steps_per_quarter = 4;
// Patterns are 4/4.
inline constexpr std::size_t quarters_per_bar = steps_per_bar / steps_per_quarter;
// The lengths the generator writes, in bars.
inline constexpr std::array<std::size_t, 4> bar_choices{1, 2, 4, 8};
// The longest pattern (an Evolve).
inline constexpr std::size_t max_bars = 64;

inline constexpr timeline::TickPosition sixteenth_ticks{timeline::ticks_per_quarter /
                                                        static_cast<std::int64_t>(steps_per_quarter)};
// Knock ignores note-offs, so notes are short: a thirty-second, which is
// also the gap between a ratchet's two hits.
inline constexpr timeline::TickPosition note_ticks{sixteenth_ticks.value / 2};
inline constexpr timeline::TickPosition bar_ticks{sixteenth_ticks.value *
                                                  static_cast<std::int64_t>(steps_per_bar)};

// What a cell plays: nothing, a hit, an accent, a ghost (a soft hit), a hit
// half the time (decided per bar when written), or a ratchet (two hits a
// thirty-second apart). The mockup writes them '.', 'x', 'X', 'g', '?', 'r'.
enum class Cell : std::uint8_t { off, hit, accent, ghost, chance, ratchet };
inline constexpr std::size_t cell_kinds = 6;

inline constexpr std::uint8_t hit_velocity = 84;
inline constexpr std::uint8_t accent_velocity = 116;
inline constexpr std::uint8_t ghost_velocity = 52;
// A ratchet's second hit is softer than its first.
inline constexpr std::uint8_t ratchet_echo_velocity = 70;
// A chance cell plays this often.
inline constexpr double chance_probability = 0.5;

using Lane = std::array<Cell, steps_per_bar>;
using Bar = std::array<Lane, lane_count>;
struct Pattern {
  std::vector<Bar> bars;
  bool operator==(const Pattern&) const = default;
};

// The cell for its mockup letter; off for anything unknown.
[[nodiscard]] Cell cell_from_letter(char letter) noexcept;
// A bar from eight lanes of sixteen letters; nullopt when the shape is wrong.
[[nodiscard]] std::optional<Bar> bar_from_rows(std::span<const std::string_view> rows);
// Cells that play something.
[[nodiscard]] std::size_t hit_count(const Pattern& pattern) noexcept;
[[nodiscard]] std::size_t hit_count(const Lane& lane) noexcept;

// ---- The generator ----------------------------------------------------------

// The grooves the generator writes, each with its own odds per sixteenth for
// low, mid and high pads: Top (ticks and shakers on the 16ths over sparse
// knocks), Underbelly (busy low parts under a quiet top), Rolling (every
// lane busy), Broken (Euclidean rhythms, rotated) and Skippy.
enum class Style : std::uint8_t { top, underbelly, rolling, broken, skippy };
inline constexpr std::size_t style_count = 5;
[[nodiscard]] std::string_view style_name(Style style) noexcept;

using Roles = std::array<knock::PadRole, lane_count>;
using Locks = std::array<bool, lane_count>;
// Each pad's part, from its sound in `values` (Knock's dense values).
[[nodiscard]] Roles roles_of(std::span<const double> values) noexcept;

struct GeneratorSettings {
  Style style = Style::top;
  // How busy (0..1): more lanes play and each plays more.
  double density = 0.5;
  // How much the off-beats are favoured over the beats (0..1).
  double syncopation = 0.4;
  // Soft extra hits on mid and high pads (0..1).
  double ghosts = 0.35;
  // High pads' hits that are ratchets (0..1).
  double ratchets = 0.2;
  // Hits that are left to chance (0..1).
  double chance = 0.15;
  // How much later bars differ from the first, and whether the last fills (0..1).
  double variation = 0.4;
  std::size_t bars = 2; // One of bar_choices.
  std::uint32_t seed = 1;
  bool operator==(const GeneratorSettings&) const = default;
};
inline constexpr GeneratorSettings default_settings{};
// Settings clamped into range, with bars rounded up to a bar choice.
[[nodiscard]] GeneratorSettings clamped(GeneratorSettings settings) noexcept;

// A fresh loop for pads with these roles.
[[nodiscard]] Pattern generate(const GeneratorSettings& settings, const Roles& roles);
// generate(), keeping the locked lanes of `current` bar for bar (its bars
// repeat when the new loop is longer).
[[nodiscard]] Pattern roll(const GeneratorSettings& settings, const Roles& roles,
                           const Pattern& current, const Locks& locked);

// Mutate: each unlocked cell changes with probability `amount` (a hit drops
// or flips accent, an empty cell may gain a hit or a chance). At least one
// cell changes while any lane is unlocked.
inline constexpr double mutate_amount = 0.15;
[[nodiscard]] Pattern mutate(const Pattern& pattern, double amount, std::uint32_t seed,
                             const Locks& locked);

// Evolve: `total_bars` of the loop repeating, mutated lightly every
// `every_bars` bars, so a long clip keeps moving. Locked lanes never change.
inline constexpr std::size_t evolve_every_bars = 4;
inline constexpr double evolve_amount = 0.06;
// The lengths Evolve writes (0 is off: the loop as it is).
inline constexpr std::array<std::size_t, 4> evolve_choices{0, 8, 16, 32};
[[nodiscard]] Pattern evolve(const Pattern& pattern, std::size_t total_bars,
                             std::size_t every_bars, double amount, std::uint32_t seed,
                             const Locks& locked);

// ---- The library -------------------------------------------------------------

// A named loop: one of the thirty from the generator with fixed settings
// (six per style), or one of a kit's own two (its top and its underbelly),
// hand-written for the first four kits and generated for the rest. `kit`
// is the Knock preset it belongs to; empty for the general loops.
struct LibraryEntry {
  std::string name;
  std::string kit;
  Style style = Style::top;
  // A hand-written loop, else nullopt and the loop is generate(settings).
  std::optional<Pattern> fixed;
  GeneratorSettings settings;
};
inline constexpr std::size_t general_loop_count = 30;
inline constexpr std::size_t loops_per_kit = 2;
// Every kit's two loops first (in preset order), then the thirty.
[[nodiscard]] const std::vector<LibraryEntry>& library();
[[nodiscard]] const LibraryEntry* find_loop(std::string_view name);
// The loop for pads with these roles.
[[nodiscard]] Pattern loop_pattern(const LibraryEntry& entry, const Roles& roles);

// ---- Into a clip ------------------------------------------------------------

struct ClipNotes {
  // Clip-local, sorted by start then key; the ids are placeholders the
  // session replaces.
  std::vector<project::MidiNote> notes;
  // Whole 4/4 bars: the pattern's.
  timeline::TickPosition length{0};
};
// Lane i plays key knock::first_key + i, each cell from its sixteenth: hits,
// accents and ghosts at their velocities; a chance cell when the seeded
// draw for its bar says so (about half); a ratchet as two notes a
// thirty-second apart. Fails for an empty or over-long pattern.
[[nodiscard]] foundation::Result<ClipNotes> to_notes(const Pattern& pattern, std::uint32_t seed);

// An audition of written notes: the first `limit` of them in frames at
// `bpm` (clamped to a playable range) and `sample_rate`, as the dry
// preview device plays a phrase.
inline constexpr double min_preview_bpm = 60.0;
inline constexpr double max_preview_bpm = 200.0;
// The tempo an audition plays at when the song's cannot be read.
inline constexpr double fallback_preview_bpm = 120.0;
[[nodiscard]] std::vector<PreviewNote> preview_notes(const ClipNotes& clip, double bpm,
                                                     std::uint32_t sample_rate, std::size_t limit);

} // namespace oscillate::application::knock_patterns
// clang-format on
