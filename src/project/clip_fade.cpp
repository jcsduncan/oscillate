#include "oscillate/project/clip_fade.hpp"

#include <algorithm>
#include <cmath>

namespace oscillate::project {
namespace {

std::uint64_t rounded_frames(double fraction, std::uint64_t duration) {
  const auto scaled = static_cast<long double>(fraction) * static_cast<long double>(duration);
  const auto whole = std::floor(scaled);
  auto count = static_cast<std::uint64_t>(whole);
  const auto remainder = scaled - whole;
  if (remainder > 0.5L || (remainder == 0.5L && (count & 1U) != 0))
    ++count;
  return count;
}

double curve_gain(double progress, FadeCurve curve) noexcept {
  const auto u = std::clamp(progress, 0.0, 1.0);
  return curve == FadeCurve::smoothstep ? u * u * (3.0 - 2.0 * u) : u;
}

} // namespace

foundation::Result<PreparedClipFades> prepare_clip_fades(const ClipFades& fades,
                                                         std::uint64_t duration_frames) {
  using foundation::ErrorCode;
  if (duration_frames == 0 || !std::isfinite(fades.in_fraction) ||
      !std::isfinite(fades.out_fraction) || fades.in_fraction < 0.0 || fades.out_fraction < 0.0 ||
      fades.in_fraction + fades.out_fraction > 1.0 ||
      (fades.in_curve != FadeCurve::linear && fades.in_curve != FadeCurve::smoothstep) ||
      (fades.out_curve != FadeCurve::linear && fades.out_curve != FadeCurve::smoothstep) ||
      (duration_frames == 1 && (fades.in_fraction > 0.0 || fades.out_fraction > 0.0)))
    return foundation::Result<PreparedClipFades>::failure(
        {ErrorCode::invalid_argument, "Invalid clip fade settings or duration"});
  const auto in_frames =
      std::min(duration_frames, rounded_frames(fades.in_fraction, duration_frames));
  const auto out_frames =
      std::min(duration_frames - in_frames, rounded_frames(fades.out_fraction, duration_frames));
  return foundation::Result<PreparedClipFades>::success(
      {duration_frames, in_frames, out_frames, fades.in_curve, fades.out_curve});
}

double clip_fade_gain(const PreparedClipFades& fades, std::uint64_t local_frame) noexcept {
  if (local_frame >= fades.duration_frames)
    return 0.0;
  double gain = 1.0;
  if (fades.in_frames != 0 && local_frame < fades.in_frames)
    gain *= curve_gain(static_cast<double>(local_frame) / static_cast<double>(fades.in_frames),
                       fades.in_curve);
  const auto remaining = fades.duration_frames - 1 - local_frame;
  if (fades.out_frames != 0 && remaining < fades.out_frames)
    gain *= curve_gain(static_cast<double>(remaining) / static_cast<double>(fades.out_frames),
                       fades.out_curve);
  return gain;
}

} // namespace oscillate::project
