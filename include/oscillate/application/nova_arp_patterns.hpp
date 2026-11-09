#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/nova/arp.hpp"
#include "oscillate/persistence/groove_file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The Nova arp's pattern library, mutate and groove (ADR-0078): factory
// patterns as data, applied to a patch's values as one edit that touches
// only the arp; seeded mutations of a pattern with per-lane locks; and a
// groove-pool template copied into the arp's groove parameters. Pure
// functions of their inputs.
namespace oscillate::application::nova_arp {

namespace nova = native_dsp::synth::nova;

inline constexpr std::string_view arp_prefix = "nova.arp.";
inline constexpr std::string_view lane_prefix = "nova.arp.lane.";

// A factory pattern. Lanes are written compactly; an empty lane keeps its
// defaults (length 16):
//   - level lanes (velocity, gate, chance, mod), one character a step:
//     '.' 0, '1'..'9' 0.1..0.9, 'x' 1;
//   - toggle lanes (tie, accent), one character a step: '.' off, 'x' on;
//   - integer lanes (note, octave, ratchet), whole numbers separated by
//     spaces;
//   - condition, names separated by spaces: '-' Always, "1:2" … "4:4",
//     "F" Fill, "!F" Not fill, "1st" First, "!1st" Not first;
//   - chord, names separated by spaces: '-' Off, "oct", "5", "tri", "7",
//     "sus".
// The lane's length is its number of steps.
struct Euclid {
  std::uint32_t hits = nova::arp_default_euclid_hits;
  std::uint32_t steps = nova::arp_default_length;
  std::uint32_t rotate = 0;
};
struct PatternSpec {
  std::string_view name;
  std::string_view category;
  nova::ArpMode mode = nova::ArpMode::up;
  std::string_view rate = "1/16"; // A nova.arp.rate choice.
  double gate = 0.6;
  double swing = 0.0;
  std::uint32_t octaves = 1;
  std::uint32_t variation = 0; // Bars, one of arp_variation_bars.
  Euclid euclid{};
  nova::ArpNoteMode note_mode = nova::ArpNoteMode::held;
  bool passing = false;
  double strum = nova::arp_default_strum;
  std::string_view note{}, octave{}, velocity{}, gate_lane{}, tie{}, ratchet{}, chance{},
      condition{}, accent{}, mod{}, chord{};
};

// The library's categories, in the panel's order.
inline constexpr std::array<std::string_view, 8> categories{"Minimal", "Deep House", "Techno",
                                                            "Acid",    "Dub Chords", "Trance Gate",
                                                            "Classic", "Polymetric"};

[[nodiscard]] std::span<const PatternSpec> factory_patterns() noexcept;

// A pattern's lanes parsed into values (cycled to every step).
[[nodiscard]] foundation::Result<std::array<nova::ArpLaneValues, nova::arp_lane_count>>
lanes_of(const PatternSpec& pattern);

// Lanes a load or a mutation leaves as they are.
using LaneLocks = std::array<bool, nova::arp_lane_count>;

// `current` with the pattern applied: the arp on, its mode, rate, gate,
// swing, octaves, note mode, passing notes, strum, Euclidean rhythm,
// variation and every unlocked lane from the pattern; Fill off and the seed
// at 0. The scale and root (the song's key), latch, retrigger, velocity
// mode and groove stay, and nothing outside nova.arp.* changes. Fails for a
// pattern that does not parse or values that do not fit the table.
[[nodiscard]] foundation::Result<std::vector<double>>
apply(const native_dsp::EngineDescriptor& table, std::span<const double> current,
      const PatternSpec& pattern, const LaneLocks& locks = {});

// A seeded variation of `base` (a pattern as loaded): with `amount` 0..1 of
// each unlocked lane's steps changed — levels nudged, toggles flipped,
// notes and octaves stepped, now and then a ratchet, a condition or a
// lane's length moved by a step or two (so the lanes drift against each
// other). nova.arp.seed takes the seed, so chance rolls change too. Seed 0
// returns `base`. Same base, seed and amount, same result.
inline constexpr double default_mutation_amount = 0.35;
[[nodiscard]] std::vector<double> mutate(const native_dsp::EngineDescriptor& table,
                                         std::span<const double> base, std::uint32_t seed,
                                         double amount, const LaneLocks& locks = {});

// `current` with a groove-pool template as the arp's groove: its first
// arp_groove_steps steps on its grid. Fails for a grid the arp lacks
// (arp_groove_grids) or an invalid template. clear_groove sets no groove.
[[nodiscard]] foundation::Result<std::vector<double>>
apply_groove(const native_dsp::EngineDescriptor& table, std::span<const double> current,
             const persistence::GrooveTemplate& groove);
[[nodiscard]] std::vector<double> clear_groove(const native_dsp::EngineDescriptor& table,
                                               std::span<const double> current);

// The arp's settings in a set of values, as the kernel reads them (for the
// panel's period readout and the tests).
[[nodiscard]] nova::ArpSettings settings_of(const native_dsp::EngineDescriptor& table,
                                            std::span<const double> values);

} // namespace oscillate::application::nova_arp
