#pragma once

#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::strata {

// Strata (ADR-0082): multiband dynamics. Up to six bands split by
// phase-compensated Linkwitz-Riley crossovers (zero latency, the bands sum
// to one allpass) or by a linear-phase FIR bank (the bands sum to the input,
// delayed). Each band has one transfer curve that compresses or expands
// above its threshold and lifts or pushes down below it, with a knee, a
// range, attack and release, makeup, the channel it works on (stereo, mid,
// side, left or right) and its key (the band, the whole input, or the
// sidechain split into the same band). OTT mode drives three bands from
// Depth, Time and Up / Down. Lookahead delays the audio against the
// detectors; Mix blends with the dry bands (delay-compensated); Delta plays
// what the dynamics change; Auto Gain matches the loudness in and out; Learn
// suggests a threshold per band from a few seconds of playing.
[[nodiscard]] const EffectEngine& engine();

inline constexpr std::size_t max_bands = 6;
inline constexpr std::size_t max_crossovers = max_bands - 1;
// The panel's spectrum: display_bands log-spaced bands from 20 Hz to 20 kHz
// (band i centred at 20 * 1000^((i + 0.5) / n)).
inline constexpr std::size_t display_bands = 96;

enum class Process : int { stereo, mid, side, left, right };
enum class Key : int { band, input, sidechain };

// One band's static curve, in dB. Above the threshold the slope is
// 1 / above (above > 1 compresses down, < 1 expands up); below it the slope
// is 1 / below (below > 1 lifts quiet sound up, < 1 pushes it further down).
// The knee blends the two slopes over knee_db around the threshold. The
// change never exceeds range_db either way. Makeup is added after.
struct Curve {
  double threshold_db = -18.0;
  double above = 2.0;
  double below = 1.0;
  double knee_db = 6.0;
  double range_db = 24.0;
  double makeup_db = 0.0;
};

struct Band {
  Curve curve{};
  double attack_ms = 10.0;
  double release_ms = 120.0;
  Process process = Process::stereo;
  Key key = Key::band;
  bool solo = false;
  bool mute = false;
  bool bypass = false;
};

// What plays: the band count and the crossovers (ascending, at least a third
// of an octave apart and below 0.45 of the rate), OTT or not.
struct Layout {
  std::size_t bands = 1;
  std::array<double, max_crossovers> crossovers{};
  bool ott = false;
};

// The static gain change in dB (before makeup) of a steady level, range
// included.
[[nodiscard]] double gain_change_db(double level_db, const Curve& curve) noexcept;
// The curve's output for a steady level, makeup included.
[[nodiscard]] inline double output_db(double level_db, const Curve& curve) noexcept {
  return level_db + gain_change_db(level_db, curve) + curve.makeup_db;
}

[[nodiscard]] Layout layout_for(std::span<const double> values, double sample_rate) noexcept;
// Band `band` (from 0) as it plays: in OTT mode, derived from Depth, Time and
// Up / Down (its solo, mute and bypass stay the band's own).
[[nodiscard]] Band band_for(std::span<const double> values, std::size_t band) noexcept;
// The plain values OTT mode runs, as the band parameters (and Bands, the
// crossovers and Mode = Bands) that reproduce it: a panel taking over a
// band writes these first, so the sound does not change.
[[nodiscard]] std::vector<synth::NamedValue> ott_values(std::span<const double> values);

// The magnitude of band `band` of the layout at `hz` (the minimum-phase
// split's; the linear-phase bank matches it in magnitude closely), for
// drawing.
[[nodiscard]] double band_magnitude(const Layout& layout, std::size_t band, double hz,
                                    double sample_rate) noexcept;

// The linear-phase bank and lookahead for these values at this rate.
struct Frames {
  std::size_t half_length = 0; // FIR taps either side of its centre
  std::size_t hop = 0;         // the bank's FFT block
  std::size_t fft = 0;
  std::uint32_t lookahead = 0;
  std::uint32_t latency = 0;
};
[[nodiscard]] Frames frames_for(std::span<const double> values, double sample_rate) noexcept;

// Telemetry: input and output peak (dBFS, held 1 s) per channel; 1 when a
// sidechain is connected; the latency (ms); the auto gain applied (dB);
// Learn's progress (0..1, 0 when off); the band count; per band its gain
// change now (dB, negative cuts, makeup excluded), its detector level
// (dBFS) and Learn's suggested threshold (dB, -200 until it has one); the
// crossovers as they play (Hz); then per display band the input and output
// spectrum (dBFS).
namespace telemetry {
inline constexpr std::size_t input_peak = 0;  // left, right
inline constexpr std::size_t output_peak = 2; // left, right
inline constexpr std::size_t keyed = 4;
inline constexpr std::size_t latency_ms = 5;
inline constexpr std::size_t auto_gain_db = 6;
inline constexpr std::size_t learn_progress = 7;
inline constexpr std::size_t band_count = 8;
inline constexpr std::size_t band_gain = 9;
inline constexpr std::size_t band_level = band_gain + max_bands;
inline constexpr std::size_t learned = band_level + max_bands;
inline constexpr std::size_t crossovers = learned + max_bands;
inline constexpr std::size_t input_spectrum = crossovers + max_crossovers;
inline constexpr std::size_t output_spectrum = input_spectrum + display_bands;
inline constexpr std::size_t size = output_spectrum + display_bands;
} // namespace telemetry

// Telemetry's floor: no level, or no suggestion yet.
inline constexpr float silent_db = -200.0F;

} // namespace oscillate::native_dsp::effects::strata
