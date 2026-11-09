#pragma once

#include "oscillate/application/sound_map.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/parameters.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The Constellation's morph and mutate for Nova (ADR-0077): several presets
// blended into one sound, every parameter in between, and seeded variations
// around a sound. Pure functions of their inputs.
namespace oscillate::application::nova_morph {

// Nova's vocabulary the rules read.
inline constexpr std::string_view fx_prefix = "nova.fx.";
inline constexpr std::string_view envelope_prefix = "nova.env.";
inline constexpr std::string_view route_prefix = "nova.mod.";
inline constexpr std::string_view route_amount_suffix = ".amount";
inline constexpr std::string_view performance_prefix = "nova.perf.";
inline constexpr std::string_view fx_media_prefix = "fx.";
inline constexpr std::string_view fx_media_suffix = ".ir";

// KEEP MINE: parts of the current sound a morph or mutation leaves alone.
struct Locks {
  bool fx = false;        // nova.fx.* and the effect chains (and their impulses).
  bool envelopes = false; // nova.env.*
  bool routes = false;    // the modulation routes and their amounts.
  bool operator==(const Locks&) const = default;
};

// One preset in a blend and its weight.
struct Part {
  const sound_map::Sound* sound = nullptr;
  double weight = 0.0;
};
// The cursor blends this many nearest stars.
inline constexpr std::size_t max_parts = 3;

// The heaviest part (the first of equals).
[[nodiscard]] std::size_t heaviest(std::span<const Part> parts) noexcept;

// Blends up to max_parts weighted sounds (weights are normalised):
//   - continuous parameters in normalised (knob) space, honouring skew;
//   - an effect slot's parameters only over the parts that hold the same
//     module in that slot as the heaviest;
//   - discrete parameters and switches from the heaviest;
//   - the document from the heaviest, with each of its route's amount
//     blended over the parts' routes of the same source to the same
//     destination (absent counts as 0);
//   - the performance controls (bend, wheel, pressure) and anything locked
//     from `current`.
// Fails for no parts, more than max_parts, a negative, non-finite or zero
// total weight, or a sound whose values do not fit the table.
[[nodiscard]] foundation::Result<sound_map::Sound> morph(const native_dsp::EngineDescriptor& table,
                                                         std::span<const Part> parts,
                                                         const sound_map::Sound& current,
                                                         Locks locks);

// Mutations: each continuous parameter moved by up to mutation_spread of its
// knob travel, uniformly, from a seed; discrete parameters, the document,
// the level, tuning, performance and global plumbing stay. Same base and
// seed, same variations.
inline constexpr std::size_t variation_count = 6;
inline constexpr double mutation_spread = 0.08;
[[nodiscard]] bool mutable_parameter(const native_dsp::ParameterDescriptor& parameter,
                                     Locks locks) noexcept;
[[nodiscard]] std::vector<sound_map::Sound> mutate(const native_dsp::EngineDescriptor& table,
                                                   const sound_map::Sound& base, std::uint64_t seed,
                                                   Locks locks = {},
                                                   std::size_t count = variation_count);

} // namespace oscillate::application::nova_morph
