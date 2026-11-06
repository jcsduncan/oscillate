#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::flyby {

// Flyby (ADR-0067): a sound travelling along a path past the listener.
// The input (summed to mono: a moving source is a point) is heard from where
// the path puts it, each ear through its own delay line read at the
// emission time, so the pitch shift is the true Doppler shift. Woodworth's
// formula gives the time between the ears and Brown and Duda's spherical
// head its shadow (Headphones), or a constant-power pan replaces both
// (Speakers). The level follows the distance law from Reference, the air
// absorbs highs by distance (ISO 9613 at 20 °C, 50% humidity), and the
// room (an Expanse inside) is heard more as the source moves away. The
// travel time is measured from the path's closest approach, so the moment
// it passes lands where it is placed.
[[nodiscard]] const EffectEngine& engine();

// The listener's head: the ears this far either side of the centre, and
// the speed of sound (m/s).
inline constexpr double head_radius = 0.0875;
inline constexpr double speed_of_sound = 343.0;
// Sync's pass lengths, 1/4 bar to 16 bars (flyby.length).
inline constexpr std::size_t length_count = 7;
// Delay lines hold this long: the farthest a path can reach, with room.
inline constexpr double max_delay_seconds = 1.0;

// Telemetry: the source now (x, y, height in metres; the listener faces
// +y), its distance (m), the pitch it is heard at (cents from the input),
// the time between the ears (µs, positive when the right ear hears first),
// each ear's level against the input (dB), the room send (0..1), the
// position along the path (0..1), the path's closest approach (m), the
// source's speed (m/s), and the input and output peaks (dBFS, held 1 s).
namespace telemetry {
inline constexpr std::size_t x = 0;
inline constexpr std::size_t y = 1;
inline constexpr std::size_t z = 2;
inline constexpr std::size_t distance = 3;
inline constexpr std::size_t pitch_cents = 4;
inline constexpr std::size_t itd_us = 5;
inline constexpr std::size_t level_left_db = 6;
inline constexpr std::size_t level_right_db = 7;
inline constexpr std::size_t room = 8;
inline constexpr std::size_t position = 9;
inline constexpr std::size_t closest = 10;
inline constexpr std::size_t speed = 11;
inline constexpr std::size_t input_peak = 12;
inline constexpr std::size_t output_peak = 13; // left, right
inline constexpr std::size_t size = output_peak + 2;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::flyby
