#pragma once

#include <QString>
#include <QVariantMap>

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <string_view>

namespace oscillate::quick {

// The device-chain strip's live view (DeviceStripBody.qml) for engines whose
// telemetry is not an output spectrum. Each scale is the one the engine's
// full panel draws on, so a strip and its panel read alike.
namespace strip_view {
// Ceiling and Press: reduction sampled at each poll, about 4 s at the
// strip's 66 ms poll; the scale hangs from 0 dB to the panel's range
// (CeilingHistory, PressReduction). A band leads when it holds at least
// this share of the detector.
inline constexpr std::size_t history_points = 60;
inline constexpr double ceiling_range_db = 12.0;
inline constexpr double press_range_db = 24.0;
inline constexpr double least_leading_share = 0.05;
// Trim: TrimPanel's meter scale and colour zones.
inline constexpr double meter_floor_db = -60.0;
inline constexpr double meter_ceiling_db = 6.0;
inline constexpr double meter_hot_db = -12.0;
inline constexpr double meter_over_db = -3.0;
// Lens: its default dB window, every second point (the louder of each pair).
inline constexpr double spectrum_floor_db = -90.0;
inline constexpr double spectrum_ceiling_db = 0.0;
inline constexpr std::size_t lens_points_per_point = 2;
// Scope: every fourth column (the outer min and max of each four).
inline constexpr std::size_t scope_columns_per_column = 4;
// Loudness: the R128 history view, -18 to +9 LU around the target.
inline constexpr double loudness_below_lu = -18.0;
inline constexpr double loudness_above_lu = 9.0;
// Shown as "0.0 dB" under this; levels at or under this read as silence.
inline constexpr double unchanged_db = 0.05;
inline constexpr double silent_below = -199.0;
} // namespace strip_view

// Decodes one engine's telemetry frame into the strip's {strip} map, which
// the strip draws by its kind (every level in dB, LU or LUFS):
// - "reduction" (Ceiling, Press): history (reduction per poll, oldest
//   first, up to `slots` of them), range_db, text ("−2.3 dB · max −4.1"),
//   bands (each reduction band's share, low to high), lead (the leading
//   band, -1 for none) and lead_text ("47–112 Hz", empty for none).
// - "meters" (Trim): input_peak, input_rms, output_peak, output_rms (left,
//   right), floor_db, ceiling_db, hot_db, over_db and text (the gain).
// - "curve" (Lens): values (the first analyser curve, low to high), floor_db,
//   ceiling_db and text (the strongest resonance, empty for none).
// - "scope" (Scope): track and key ((min, max) per column, key empty when
//   unkeyed), columns, write_column (the beam), keyed and text (the
//   correlation).
// - "loudness" (Loudness): history (short-term LUFS, oldest first; at or
//   under silent_below for none), target, below_lu, above_lu, text
//   (short-term and integrated), peak_text (true peak) and over (the true
//   peak is above the Ceiling parameter).
// The reduction history is the view's own, sampled at each decode.
class DeviceStripView {
public:
  // A parameter's current value by id.
  using ValueOf = std::function<double(std::string_view)>;

  // Empty when the engine has no strip view here or the frame is short.
  [[nodiscard]] QVariantMap decode(std::string_view engine, std::span<const float> frame,
                                   const ValueOf& value);
  // Forgets the reduction history (nothing plays).
  void clear() noexcept;

  // Text the strip shows, in the panels' style ("−" for minus).
  [[nodiscard]] static QString signed_db(double db);
  [[nodiscard]] static QString band_text(std::size_t band);

private:
  [[nodiscard]] QVariantMap reduction(std::span<const float> frame, std::size_t reduction_at,
                                      std::size_t bands_at, double range_db);

  std::array<double, strip_view::history_points> history_{};
  std::size_t next_ = 0;
  std::size_t filled_ = 0;
};

} // namespace oscillate::quick
