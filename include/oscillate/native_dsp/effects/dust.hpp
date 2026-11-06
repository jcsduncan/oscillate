#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oscillate::native_dsp::effects::dust {

// Dust (ADR-0069): a bed of background texture, and age for what passes
// through. Eight layers, all synthesised and seeded so an export sounds as
// playback does: vinyl (crackle, pops and scratches that come round with
// the record, surface noise, rumble), tape (hiss, dropouts), electrical
// hum and buzz, radio static (with a tempo-locked interference gate),
// rain, wind, room tone and machines. The input is aged on its way
// through: a worn record's band and drive, wow and flutter, lows folded to
// mono. The bed ducks under a sidechain or the notes of a keying track, so
// it breathes with the kick. With nothing playing in, it sounds alone.
[[nodiscard]] const EffectEngine& engine();

// The layers, in table and telemetry order.
enum class Layer : std::uint8_t { vinyl, tape, hum, radio, rain, wind, room, machine };
inline constexpr std::size_t layer_count = 8;

// Each layer alone at Level 0 dB and its default settings sits about here
// (RMS, dBFS): a bed under a mix, felt more than heard. (At -18 dBFS, the
// first calibration, every preset was far too loud.)
inline constexpr double layer_reference_dbfs = -40.0;
// The record's speeds (dust.vinyl.speed), in revolutions a minute.
inline constexpr std::array<double, 3> rpm{100.0 / 3.0, 45.0, 78.0};
// Crackle c sets the pops a second:
// pop_rate_max * (pop_rate_span^c - 1) / (pop_rate_span - 1), none at 0.
inline constexpr double pop_rate_max = 40.0;
inline constexpr double pop_rate_span = 80.0;
// The mains (dust.hum.mains), in hertz.
inline constexpr std::array<double, 2> mains_hz{50.0, 60.0};
// The radio's gate (dust.radio.gate): each step's length in beats (0: off).
inline constexpr std::array<double, 4> gate_beats{0.0, 1.0, 0.5, 0.25};
// An open step holds for this share of its length, then closes.
inline constexpr double gate_hold = 0.55;
// The aged input runs through a delay modulated about this centre (wow and
// flutter), so the effect reports it as its latency and the host lines the
// input up; the bed is generated on the same, compensated clock.
inline constexpr double latency_seconds = 0.0025;
// Wow (slow, about wow_hz) and flutter (6-10 Hz, irregular) at full: the
// largest pitch deviation each reaches, as a fraction of the pitch.
inline constexpr double wow_hz = 0.5;
inline constexpr double wow_max_deviation = 0.006;
inline constexpr double flutter_max_deviation = 0.0015;
// The sidechain duck: a key peaking at duck_full_dbfs or louder ducks the
// bed by the full Duck amount, fading to none at duck_floor_dbfs.
inline constexpr double duck_full_dbfs = -12.0;
inline constexpr double duck_floor_dbfs = -36.0;
// Meters never read below this.
inline constexpr double meter_floor_db = -120.0;

// Telemetry: each layer's output level (dBFS, >= -120, held 300 ms), the
// duck's gain reduction (dB, >= 0), the record's rotation (0..1, for the
// spinning record), the count of crackle pops since reset (wraps at 2^24,
// for the panel's sparks), and the output peaks (dBFS, held 1 s).
namespace telemetry {
inline constexpr std::size_t first_layer = 0;
inline constexpr std::size_t duck_db = first_layer + layer_count;
inline constexpr std::size_t rotation = duck_db + 1;
inline constexpr std::size_t pops = rotation + 1;
inline constexpr std::size_t output_peak = pops + 1; // left, right
inline constexpr std::size_t size = output_peak + 2;
} // namespace telemetry

} // namespace oscillate::native_dsp::effects::dust
