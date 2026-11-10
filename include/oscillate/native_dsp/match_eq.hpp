#pragma once

#include "oscillate/native_dsp/eq.hpp"
#include "oscillate/native_dsp/tonal_analysis.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"

#include <array>
#include <cstddef>
#include <span>

// Match EQ (ADR-0120): the correction that moves a mix's long-term
// spectrum to a reference's or a target's, and the fewest Shape bands that
// draw it. Control or worker thread only; nothing here runs on the audio
// callback.
namespace oscillate::native_dsp::match_eq {

using tonal::FineCurve;

struct Settings {
  double amount = 1.0; // 0..1 of the correction.
  double smoothing_octaves = 1.0 / 3.0;
  double low_hz = 30.0;
  double high_hz = 16'000.0;
  double max_boost_db = 6.0;
  double max_cut_db = 6.0;
};

// Outside low..high the correction fades to nothing over this much.
inline constexpr double fade_octaves = 0.5;

// The correction in dB at each fine band. `mix_db` and `reference_db` are
// fine levels (tonal::fine_levels) at any gain: each is loudness-normalised
// (tonal::fine_loudness_db) before the difference, reference minus mix.
// The difference is smoothed with a Hann window whose half-power width is
// smoothing_octaves (bands silent in either curve take no part), scaled by
// amount, held within max_boost_db and max_cut_db, and faded out beyond
// low_hz and high_hz.
[[nodiscard]] FineCurve correction(const FineCurve& mix_db, const FineCurve& reference_db,
                                   const Settings& settings) noexcept;

// A target's median as fine levels: linear in log frequency between band
// centres, flat beyond the first and last.
[[nodiscard]] FineCurve target_levels(const tonal::Target& target) noexcept;

// What the fit uses: Shape's eight bands, its response as designed at this
// rate.
inline constexpr std::size_t max_bands = 8;
inline constexpr double fit_rate = 48'000.0;
// The fit stops adding bands once its RMS error over low..high is at most
// this.
inline constexpr double fit_tolerance_db = 0.25;

// One Shape band: a peak, a low shelf or a high shelf. `shape` is the Q
// (a shelf's slope).
struct Band {
  EqBandKind kind = EqBandKind::peaking;
  double hz = 1'000.0;
  double shape = 1.0;
  double gain_db = 0.0;
};

struct Fit {
  std::array<Band, max_bands> band{};
  std::size_t size = 0;
  FineCurve response_db{};
  // Fit error over low..high: RMS and largest, in dB.
  double rms_db = 0.0;
  double max_db = 0.0;
};

// The fewest bands, up to max_bands, whose response draws `curve_db`
// within fit_tolerance_db RMS over low..high. Bands are added one at a
// time where the error is largest (a peak, or a shelf at either end, at
// most one of each), and every band is refined together after each
// addition (Levenberg-Marquardt over frequency, shape and gain). Peaks keep
// a Q from min_q to max_q, shelves a slope from min_slope to 1.
inline constexpr double min_q = 0.3;
inline constexpr double max_q = 8.0;
inline constexpr double min_slope = 0.3;
[[nodiscard]] Fit fit(const FineCurve& curve_db, const Settings& settings);

// The bands' response in dB at each fine band, as Shape plays them at
// fit_rate.
[[nodiscard]] FineCurve response_db(std::span<const Band> bands);

} // namespace oscillate::native_dsp::match_eq
