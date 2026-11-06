#pragma once

#include "oscillate/native_dsp/synth/modal_dsp.hpp"

#include <cstddef>
#include <cstdint>

// Knock's strikers (ADR-0068): what hits the resonator, as physics. Each
// type is a mass with its own contact law; Hardness sets the stiffness so
// that, at velocity 100, the contact lasts from about 6.5 ms (soft) to
// 0.23 ms (hard), longer for a finger or a brush. The kernel and its tests
// share these.
namespace oscillate::native_dsp::synth::knock {

enum class StrikerType : std::uint8_t { mallet, stick, finger, brush };
inline constexpr std::size_t striker_type_count = 4;

// The velocity a striker is designed at, as MIDI and as 0..1.
inline constexpr std::uint8_t reference_velocity = 100;
inline constexpr double reference_velocity_unit = reference_velocity / 127.0;

// The contact time a hardness aims at, at the reference velocity.
[[nodiscard]] double contact_target_seconds(StrikerType type, double hardness) noexcept;
// Hardness after velocity (0..1) at a velocity sensitivity (0..1): harder
// hits press a little harder.
[[nodiscard]] double velocity_hardness(double hardness, double velocity,
                                       double sensitivity) noexcept;
// The impact speed (m/s) for a velocity (0..1) at a sensitivity (0..1).
[[nodiscard]] double strike_speed(double velocity, double sensitivity) noexcept;
// A striker of a type and hardness, ready to launch.
[[nodiscard]] modal::Striker make_striker(StrikerType type, double hardness) noexcept;
// The modal mass of each resonator mode for a striker type, given the sum
// of the squared mode shapes at the strike point: the surface then meets
// the striker as a mass `surface_mass_ratio` times its own, however many
// modes ring.
inline constexpr double surface_mass_ratio = 40.0;
[[nodiscard]] double surface_mass(StrikerType type, double shape_power) noexcept;

} // namespace oscillate::native_dsp::synth::knock
