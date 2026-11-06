#include "synth.hpp"

#include "spectrum.hpp"

#include <algorithm>
#include <cmath>

namespace oscillate::clay_dsp::detail {
namespace {

struct Cubic {
  double theta = 0.0;
  double omega = 0.0;
  double alpha = 0.0;
  double beta = 0.0;
  double amplitude = 0.0;
  double slope = 0.0; // amplitude per frame
};

[[nodiscard]] Cubic cubic(double theta0, double omega0, double theta1, double omega1, double a0,
                          double a1, double length) noexcept {
  // McAulay & Quatieri: pick the unwrapping M that makes the phase smoothest.
  const double m =
      std::round(((theta0 + omega0 * length - theta1) + (omega1 - omega0) * length / 2.0) / two_pi);
  const double gap = theta1 + two_pi * m - theta0 - omega0 * length;
  const double t2 = length * length;
  return {theta0,
          omega0,
          3.0 / t2 * gap - (omega1 - omega0) / length,
          -2.0 / (t2 * length) * gap + (omega1 - omega0) / t2,
          a0,
          (a1 - a0) / length};
}

[[nodiscard]] Cubic hold(double theta, double omega, double amplitude) noexcept {
  return {theta, omega, 0.0, 0.0, amplitude, 0.0};
}

[[nodiscard]] double read_cubic(const std::vector<float>& source, double position) noexcept {
  const double floor_position = std::floor(position);
  const auto i = static_cast<std::int64_t>(floor_position);
  const double t = position - floor_position;
  const auto size = static_cast<std::int64_t>(source.size());
  const auto at = [&](std::int64_t k) -> double {
    return k >= 0 && k < size ? source[static_cast<std::size_t>(k)] : 0.0;
  };
  if (t == 0.0)
    return at(i);
  const double p0 = at(i - 1);
  const double p1 = at(i);
  const double p2 = at(i + 1);
  const double p3 = at(i + 2);
  // Catmull-Rom.
  return p1 + 0.5 * t *
                  (p2 - p0 +
                   t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
}

} // namespace

double edge_gain(double frame, double start, double end, double fade) noexcept {
  if (fade <= 0.0)
    return frame >= start && frame < end ? 1.0 : 0.0;
  const double half = fade / 2.0;
  double gain = 1.0;
  if (frame < start - half)
    return 0.0;
  if (frame < start + half)
    gain *= 0.5 - 0.5 * std::cos(pi * (frame - (start - half)) / fade);
  if (frame >= end + half)
    return 0.0;
  if (frame >= end - half)
    gain *= 0.5 + 0.5 * std::cos(pi * (frame - (end - half)) / fade);
  return gain;
}

std::int64_t PartialTrack::first_frame() const noexcept {
  return static_cast<std::int64_t>(std::floor(start - fade / 2.0));
}

std::int64_t PartialTrack::last_frame() const noexcept {
  return static_cast<std::int64_t>(std::ceil(end + fade / 2.0)) + 1;
}

void add_track(const PartialTrack& track, std::int64_t first, std::span<float> out, double gain) {
  if (track.hops == 0 || track.partials == 0 || out.empty())
    return;
  const auto lo = std::max(first, track.first_frame());
  const auto hi = std::min(first + static_cast<std::int64_t>(out.size()), track.last_frame());
  if (lo >= hi)
    return;
  const auto count = static_cast<std::size_t>(hi - lo);
  std::vector<double> sum(count, 0.0);
  std::vector<Cubic> coefficients(track.partials);
  const auto hop = track.hop;
  const auto last_hop = static_cast<std::int64_t>(track.hops) - 1;
  const auto centre = [&](std::int64_t j) { return track.first_centre + j * hop; };
  // Segment s covers [centre(s), centre(s + 1)); s = -1 is everything before
  // the first hop and s = last_hop everything after the last.
  auto segment = [&](std::int64_t frame) -> std::int64_t {
    if (frame < track.first_centre)
      return -1;
    return std::min(last_hop, (frame - track.first_centre) / hop);
  };
  std::int64_t frame = lo;
  while (frame < hi) {
    const auto s = segment(frame);
    const auto from = s < 0 ? track.first_centre : centre(s);
    std::int64_t until = hi;
    if (s < 0)
      until = std::min(hi, track.first_centre);
    else if (s < last_hop)
      until = std::min(hi, centre(s + 1));
    for (std::size_t h = 0; h < track.partials; ++h) {
      if (s < 0 || s == last_hop) {
        const auto j = static_cast<std::size_t>(s < 0 ? 0 : last_hop) * track.partials + h;
        coefficients[h] = hold(track.phase[j], track.omega[j], track.amplitude[j]);
      } else {
        const auto j0 = static_cast<std::size_t>(s) * track.partials + h;
        const auto j1 = j0 + track.partials;
        coefficients[h] = cubic(track.phase[j0], track.omega[j0], track.phase[j1], track.omega[j1],
                                track.amplitude[j0], track.amplitude[j1], static_cast<double>(hop));
      }
    }
    for (std::size_t h = 0; h < track.partials; ++h) {
      const auto& c = coefficients[h];
      if (c.amplitude == 0.0 && c.slope == 0.0)
        continue;
      for (auto t = frame; t < until; ++t) {
        const auto tau = static_cast<double>(t - from);
        const double theta = c.theta + tau * (c.omega + tau * (c.alpha + tau * c.beta));
        const double amplitude = c.amplitude + c.slope * tau;
        sum[static_cast<std::size_t>(t - lo)] += amplitude * std::cos(theta);
      }
    }
    frame = until;
  }
  for (std::size_t i = 0; i < count; ++i) {
    const auto t = lo + static_cast<std::int64_t>(i);
    const double g = edge_gain(static_cast<double>(t), track.start, track.end, track.fade);
    out[static_cast<std::size_t>(t - first)] += static_cast<float>(gain * g * sum[i]);
  }
}

PartialTrack original_track(const Note& note, std::int64_t hop, std::uint32_t sample_rate,
                            double fade) {
  PartialTrack track;
  track.first_centre = note.first_hop * hop;
  track.hop = hop;
  track.hops = note.hops();
  track.partials = note.partial_count;
  track.start = static_cast<double>(note.start);
  track.end = static_cast<double>(note.end);
  track.fade = fade;
  const auto size = track.hops * track.partials;
  track.omega.resize(size);
  track.amplitude.resize(size);
  track.phase.resize(size);
  const double to_omega = two_pi / static_cast<double>(sample_rate);
  for (std::size_t i = 0; i < size && i < note.partials.size(); ++i) {
    track.omega[i] = note.partials[i].frequency * to_omega;
    track.amplitude[i] = note.partials[i].amplitude;
    track.phase[i] = note.partials[i].phase;
  }
  return track;
}

void add_segment(const GrainSegment& segment, std::int64_t first, std::span<float> out,
                 double gain) {
  if (out.empty() || segment.grain_hop <= 0)
    return;
  const auto half_fade = static_cast<std::int64_t>(std::ceil(segment.fade / 2.0));
  const auto lo = std::max(first, segment.out_start - half_fade);
  const auto hi =
      std::min(first + static_cast<std::int64_t>(out.size()), segment.out_end + half_fade + 1);
  const auto grain = segment.grain_hop;
  const auto grain_d = static_cast<double>(grain);
  for (auto t = lo; t < hi; ++t) {
    const auto offset = t - segment.out_start;
    // floor division, so grains sit on one grid whatever the block.
    auto k = offset / grain;
    if (offset < 0 && offset % grain != 0)
      --k;
    double value = 0.0;
    for (const auto g : {k, k + 1}) {
      const auto centre = segment.out_start + g * grain;
      const double weight = 0.5 + 0.5 * std::cos(pi * static_cast<double>(t - centre) / grain_d);
      const double position = segment.source_at_start +
                              static_cast<double>(centre - segment.out_start) * segment.time_rate +
                              static_cast<double>(t - centre) * segment.pitch_rate;
      value += weight * read_cubic(segment.source, position);
    }
    const double g = edge_gain(static_cast<double>(t), static_cast<double>(segment.out_start),
                               static_cast<double>(segment.out_end), segment.fade);
    out[static_cast<std::size_t>(t - first)] += static_cast<float>(gain * g * value);
  }
}

} // namespace oscillate::clay_dsp::detail
