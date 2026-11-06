#pragma once

#include "oscillate/project/model.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// Velocity editing for the piano roll's velocity lane: pure functions that
// turn a gesture into new velocities for chosen notes. Timing and note
// identity never change; the session applies the result as one undo step.
namespace oscillate::application {

using VelocityChange = std::pair<project::NoteId, std::uint8_t>;

[[nodiscard]] std::uint8_t clamp_velocity(double value) noexcept;

// A line drawn across the stems: each chosen note whose start lies between
// the line's ends takes the line's value at its start (a crescendo or
// diminuendo in one gesture).
[[nodiscard]] std::vector<VelocityChange> velocity_line(std::span<const project::MidiNote> notes,
                                                        std::span<const project::NoteId> scope,
                                                        std::int64_t from_tick,
                                                        double from_velocity, std::int64_t to_tick,
                                                        double to_velocity);

// Humanise: small, centre-weighted variation around each velocity. The
// spread scales with strength (0..1, up to +/-16 at full strength) and is
// tighter for accents, so the intended dynamics survive.
[[nodiscard]] std::vector<VelocityChange>
humanize_velocities(std::span<const project::MidiNote> notes,
                    std::span<const project::NoteId> scope, double strength, std::uint64_t seed);

// Randomise: each chosen note takes a velocity drawn evenly from
// [minimum, maximum].
[[nodiscard]] std::vector<VelocityChange>
randomize_velocities(std::span<const project::MidiNote> notes,
                     std::span<const project::NoteId> scope, int minimum, int maximum,
                     std::uint64_t seed);

} // namespace oscillate::application
