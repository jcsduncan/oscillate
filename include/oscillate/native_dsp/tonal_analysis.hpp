#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/tonal_targets.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Measuring tonal curves (ADR-0120): the band meter Lens runs and the
// target tool runs offline, so a mix in Lens and a reference measured from
// a file read alike; building targets from measured curves; and the target
// file format.
namespace oscillate::native_dsp::tonal {

// The fine grid: each grid band split into four twelfth-octave bands, in
// order, so four fine bands sum to one grid band exactly.
inline constexpr std::size_t fine_per_band = 4;
inline constexpr std::size_t fine_bands = bands * fine_per_band;
[[nodiscard]] double fine_hz(std::size_t fine) noexcept;
[[nodiscard]] double fine_low_hz(std::size_t fine) noexcept;
[[nodiscard]] double fine_high_hz(std::size_t fine) noexcept;
using FineCurve = std::array<double, fine_bands>;

// Linear band powers to a grid curve in dB (each grid band the sum of its
// four fine bands), and fine powers to dB per fine band. Silent bands read
// dsp::silent_db.
[[nodiscard]] Curve grid_levels(const FineCurve& power) noexcept;
[[nodiscard]] FineCurve fine_levels(const FineCurve& power) noexcept;
// A fine curve's loudness as loudness_db reads a grid curve, with the
// K-weighting at each fine band's centre.
[[nodiscard]] double fine_loudness_db(const FineCurve& levels) noexcept;

// The long-term average power of two signals in every fine band: Welch's
// method with a periodic Hann window of about a third of a second (16384
// frames at 48 kHz, the nearest power of two at other rates), half
// overlapped. Band powers are one-sided and sum to the signal's mean
// square. The two signals share one complex transform.
// Construction allocates (control thread); push, reset and the reads
// allocate nothing and are safe on the audio callback.
inline constexpr double meter_design_rate = 48'000.0;
inline constexpr std::size_t meter_window_frames = 16'384;
// One window is analysed per hop, half a window: about this long at any rate.
inline constexpr double meter_hop_seconds =
    static_cast<double>(meter_window_frames) / 2.0 / meter_design_rate;
class BandMeter {
public:
  explicit BandMeter(double sample_rate);
  ~BandMeter();
  BandMeter(BandMeter&&) noexcept;
  BandMeter& operator=(BandMeter&&) noexcept;
  BandMeter(const BandMeter&) = delete;
  BandMeter& operator=(const BandMeter&) = delete;

  void push(double a, double b) noexcept;
  void reset() noexcept;
  // Windows analysed since the last reset; the mean band powers over them
  // (zero before the first).
  [[nodiscard]] std::uint64_t windows() const noexcept { return windows_; }
  [[nodiscard]] const FineCurve& mean_a() const noexcept { return mean_[0]; }
  [[nodiscard]] const FineCurve& mean_b() const noexcept { return mean_[1]; }
  [[nodiscard]] std::size_t window_frames() const noexcept { return size_; }

private:
  void analyse() noexcept;

  struct Plan;
  std::size_t size_ = 0;
  std::size_t hop_ = 0;
  std::unique_ptr<Plan> plan_;
  std::vector<double> window_;
  std::array<std::vector<double>, 2> ring_;
  std::vector<std::complex<double>> work_;
  std::array<std::vector<double>, 2> cumulative_;
  std::array<double, fine_bands> low_bin_{};
  std::array<double, fine_bands> high_bin_{};
  double scale_ = 1.0;
  std::size_t write_ = 0;
  std::size_t filled_ = 0;
  std::size_t since_ = 0;
  std::uint64_t windows_ = 0;
  std::array<FineCurve, 2> mean_{};
};

// A target from references' normalised curves: per band the median and
// the 25th and 75th percentiles (linear between order statistics), the
// same whatever order the curves come in. Needs at least one curve.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the id, then the name shown.
[[nodiscard]] foundation::Result<Target> build_target(std::string id, std::string name,
                                                      std::span<const Curve> curves);
// The value at fraction `p` (0..1) of `values`, linear between order
// statistics. `values` is reordered.
[[nodiscard]] double percentile(std::span<double> values, double p) noexcept;
inline constexpr double lower_quartile = 0.25;
inline constexpr double upper_quartile = 0.75;

// Target files (JSON, UTF-8):
//   {"format": "oscillate-tonal-target", "version": 1, "name": "...",
//    "references": N, "centres_hz": [29 band centres],
//    "median_db": [29], "low_db": [29], "high_db": [29]}
// Levels are written to a hundredth of a dB. parse_target checks the
// format and version, the name (1 to max_name_bytes), references (at least
// 1), that the centres are this grid's (within centre_tolerance), and that
// every level is finite, within level_floor_db..level_ceiling_db, with
// low <= median <= high. `id` is not stored: it comes from where the file
// lives.
inline constexpr std::string_view file_format = "oscillate-tonal-target";
inline constexpr int file_version = 1;
inline constexpr std::size_t max_file_bytes = 64 * 1024;
inline constexpr std::size_t max_name_bytes = 64;
inline constexpr double centre_tolerance = 0.005; // Relative.
inline constexpr double level_floor_db = -120.0;
inline constexpr double level_ceiling_db = 40.0;
[[nodiscard]] std::string serialise_target(const Target& target);
[[nodiscard]] foundation::Result<Target> parse_target(std::string_view text, std::string id);

} // namespace oscillate::native_dsp::tonal
