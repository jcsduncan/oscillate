#include "oscillate/automation/curve.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace oscillate::automation {
namespace {

[[nodiscard]] bool valid_shape(Shape shape) {
  return static_cast<unsigned>(shape) <= static_cast<unsigned>(Shape::bezier);
}

[[nodiscard]] bool valid_handles(const BezierHandles& h) {
  return std::isfinite(h.x1) && std::isfinite(h.y1) && std::isfinite(h.x2) && std::isfinite(h.y2) &&
         h.x1 >= 0.0 && h.x1 <= h.x2 && h.x2 <= 1.0 && h.y1 >= 0.0 && h.y1 <= 1.0 && h.y2 >= 0.0 &&
         h.y2 <= 1.0;
}

[[nodiscard]] bool valid_curve(const Curve& curve, const ParameterDescriptor& descriptor) {
  if (curve.timebase != Timebase::musical && curve.timebase != Timebase::elapsed) {
    return false;
  }
  for (std::size_t index = 0; index < curve.points.size(); ++index) {
    const auto& point = curve.points[index];
    if (point.coordinate < 0 || !std::isfinite(point.value) || point.value < descriptor.minimum ||
        point.value > descriptor.maximum ||
        (descriptor.discrete && std::trunc(point.value) != point.value) ||
        !valid_shape(point.to_next) || !valid_handles(point.handles) ||
        (index != 0 && point.coordinate <= curve.points[index - 1].coordinate)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] double cubic(double t, double first, double second) {
  const double remaining = 1.0 - t;
  return 3.0 * remaining * remaining * t * first + 3.0 * remaining * t * t * second + t * t * t;
}

[[nodiscard]] double to_decibels(double gain) noexcept {
  return gain > 0.0 ? std::max(decibel_floor, 20.0 * std::log10(gain)) : decibel_floor;
}

} // namespace

double shape_progress(Shape shape, double u, const BezierHandles& handles) noexcept {
  switch (shape) {
  case Shape::hold:
    return 0.0;
  case Shape::linear:
    return u;
  case Shape::quadratic:
    return u * u;
  case Shape::exponential:
    // Fixed normalized exponential: exact endpoints with a 4.0 curvature.
    return std::expm1(4.0 * u) / std::expm1(4.0);
  case Shape::ease_in_out:
    return u * u * (3.0 - 2.0 * u);
  case Shape::bezier: {
    double low = 0.0;
    double high = 1.0;
    for (int iteration = 0; iteration < 24; ++iteration) {
      const double middle = (low + high) * 0.5;
      if (cubic(middle, handles.x1, handles.x2) < u) {
        low = middle;
      } else {
        high = middle;
      }
    }
    return cubic((low + high) * 0.5, handles.y1, handles.y2);
  }
  }
  return 0.0; // create() rejects unknown shapes.
}

double blend(Warp warp, double from, double to, double progress) noexcept {
  switch (warp) {
  case Warp::logarithmic:
    if (from > 0.0 && to > 0.0)
      return std::exp(std::lerp(std::log(from), std::log(to), progress));
    break;
  case Warp::decibel: {
    const double db = std::lerp(to_decibels(from), to_decibels(to), progress);
    return db <= decibel_floor ? 0.0 : std::pow(10.0, db / 20.0);
  }
  case Warp::linear:
    break;
  }
  return std::lerp(from, to, progress);
}

foundation::Result<PreparedAutomation> PreparedAutomation::create(
    ParameterDescriptor descriptor, std::optional<Curve> track, std::optional<ClipEnvelope> clip,
    std::shared_ptr<const timeline::TempoMap> tempo, std::uint32_t sample_rate) {
  const bool valid_descriptor =
      std::isfinite(descriptor.minimum) && std::isfinite(descriptor.maximum) &&
      std::isfinite(descriptor.default_value) && descriptor.minimum < descriptor.maximum &&
      std::isfinite(descriptor.maximum - descriptor.minimum) &&
      descriptor.default_value >= descriptor.minimum &&
      descriptor.default_value <= descriptor.maximum &&
      static_cast<unsigned>(descriptor.warp) <= static_cast<unsigned>(Warp::decibel) &&
      (descriptor.warp != Warp::logarithmic || descriptor.minimum > 0.0) &&
      (descriptor.warp != Warp::decibel || descriptor.minimum >= 0.0) &&
      (!descriptor.discrete || (std::trunc(descriptor.minimum) == descriptor.minimum &&
                                std::trunc(descriptor.maximum) == descriptor.maximum &&
                                std::trunc(descriptor.default_value) == descriptor.default_value));
  if (!valid_descriptor || tempo == nullptr || !timeline::supported_sample_rate(sample_rate) ||
      (track && !valid_curve(*track, descriptor)) ||
      (clip &&
       (clip->curve.points.empty() || !valid_curve(clip->curve, descriptor) ||
        clip->active_begin.value < 0 || clip->active_end.value <= clip->active_begin.value))) {
    return foundation::Result<PreparedAutomation>::failure(
        {foundation::ErrorCode::invalid_argument, "invalid automation descriptor or curve"});
  }
  return foundation::Result<PreparedAutomation>::success(PreparedAutomation(
      descriptor, std::move(track), std::move(clip), std::move(tempo), sample_rate));
}

double PreparedAutomation::evaluate_curve(const Curve& curve, std::int64_t coordinate) const {
  if (curve.points.empty()) {
    return descriptor_.default_value;
  }
  const auto next = std::ranges::upper_bound(curve.points, coordinate, {},
                                             [](const Point& point) { return point.coordinate; });
  if (next == curve.points.begin()) {
    return next->value;
  }
  const auto& left = *(next - 1);
  if (next == curve.points.end() || coordinate == left.coordinate || descriptor_.discrete ||
      left.to_next == Shape::hold) {
    return left.value;
  }
  const auto& right = *next;
  const double u = static_cast<double>(coordinate - left.coordinate) /
                   static_cast<double>(right.coordinate - left.coordinate);
  const double progress = shape_progress(left.to_next, u, left.handles);
  return std::clamp(blend(descriptor_.warp, left.value, right.value, progress), descriptor_.minimum,
                    descriptor_.maximum);
}

bool PreparedAutomation::fill_musical_ticks(timeline::SamplePosition first,
                                            std::span<timeline::TickPosition> output) const {
  if (first.value < 0 ||
      output.size() >
          static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() - first.value)) {
    return false;
  }
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto sample = timeline::SamplePosition{first.value + static_cast<std::int64_t>(index)};
    timeline::TickPosition tick{};
    if (!tempo_->sample_to_tick_rt(sample, sample_rate_, tick)) {
      return false;
    }
    output[index] = tick;
  }
  return true;
}

bool PreparedAutomation::evaluate_block(timeline::SamplePosition first,
                                        std::span<const timeline::TickPosition> ticks,
                                        std::span<double> output) const {
  if (ticks.size() != output.size() || first.value < 0 ||
      output.size() >
          static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() - first.value)) {
    return false;
  }
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto sample = timeline::SamplePosition{first.value + static_cast<std::int64_t>(index)};
    const Curve* selected = track_ ? &*track_ : nullptr;
    if (clip_ && sample.value >= clip_->active_begin.value &&
        sample.value < clip_->active_end.value) {
      selected = &clip_->curve;
    }
    if (selected == nullptr) {
      output[index] = descriptor_.default_value;
      continue;
    }
    const std::int64_t coordinate =
        selected->timebase == Timebase::musical ? ticks[index].value : sample.value;
    output[index] = evaluate_curve(*selected, coordinate);
  }
  return true;
}

bool PreparedAutomation::evaluate_block(timeline::SamplePosition first,
                                        std::span<double> output) const {
  if (first.value < 0 ||
      output.size() >
          static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() - first.value)) {
    return false;
  }
  for (std::size_t index = 0; index < output.size(); ++index) {
    const auto sample = timeline::SamplePosition{first.value + static_cast<std::int64_t>(index)};
    const Curve* selected = track_ ? &*track_ : nullptr;
    if (clip_ && sample.value >= clip_->active_begin.value &&
        sample.value < clip_->active_end.value) {
      selected = &clip_->curve;
    }
    if (selected == nullptr) {
      output[index] = descriptor_.default_value;
      continue;
    }
    std::int64_t coordinate = sample.value;
    if (selected->timebase == Timebase::musical) {
      timeline::TickPosition tick{};
      if (!tempo_->sample_to_tick_rt(sample, sample_rate_, tick)) {
        return false;
      }
      coordinate = tick.value;
    }
    output[index] = evaluate_curve(*selected, coordinate);
  }
  return true;
}

} // namespace oscillate::automation
