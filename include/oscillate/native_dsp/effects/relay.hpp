#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <cstddef>

namespace oscillate::native_dsp::effects::relay {

// Relay (ADR-0040): a stereo, ping-pong or mono delay up to four seconds,
// free or tempo-synced, with a right-channel offset. Digital crossfades
// between read heads when the time changes, so the pitch never bends; Tape
// and Analog glide the time and bend it like the hardware. The feedback
// loop holds low and high cuts, antialiased saturation (tape tanh, analog
// soft clip, digital clean below 0 dBFS), the analog line's clock-dependent
// darkening and an optional diffusion chain that smears each repeat
// further. Wow and flutter, ducking under the input and wet width follow.
[[nodiscard]] const EffectEngine& engine();

inline constexpr double max_seconds = 4.0;

// Telemetry: each channel's current delay (ms), the ducking gain (dB) and
// the wet level (RMS, smoothed over 300 ms).
namespace telemetry {
inline constexpr std::size_t time_left_ms = 0;
inline constexpr std::size_t time_right_ms = 1;
inline constexpr std::size_t duck_db = 2;
inline constexpr std::size_t wet_rms = 3;
inline constexpr std::size_t size = 4;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::relay
