#include "automation_projection.hpp"

#include "saved_processing.hpp"

#include <QVariantList>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>

namespace oscillate::quick::detail {

QVariantMap automation_lane_projection(const project::Snapshot& snapshot,
                                       const project::AutomationLane& lane, std::size_t max_points,
                                       const project::Placement* placement) {
  std::optional<std::int64_t> anchor = 0;
  std::optional<std::int64_t> axis_duration;
  QVariantList axis_map;
  if (placement != nullptr) {
    if (lane.timebase == project::AutomationTimebase::musical) {
      if (const auto* musical = std::get_if<project::MusicalInterval>(placement)) {
        anchor = musical->start.value;
      } else {
        const auto start = std::get<project::ElapsedInterval>(*placement).start;
        auto tick = snapshot.tempo.sample_to_tick(start, snapshot.sample_rate);
        anchor = tick ? std::optional<std::int64_t>(tick.value().value) : std::nullopt;
      }
    } else if (const auto* elapsed = std::get_if<project::ElapsedInterval>(placement)) {
      anchor = elapsed->start.value;
    } else {
      const auto start = std::get<project::MusicalInterval>(*placement).start;
      auto sample = snapshot.tempo.tick_to_sample(start, snapshot.sample_rate);
      anchor = sample ? std::optional<std::int64_t>(sample.value().value) : std::nullopt;
    }
    // A segment's interpolation parameter belongs to the lane timebase. When
    // the clip uses the other timebase, its x axis is nonlinear under a tempo
    // ramp. Project a bounded lookup through the authoritative tempo map once
    // per clip, independently of the number of automation points.
    const bool musical_lane = lane.timebase == project::AutomationTimebase::musical;
    const bool musical_clip = std::holds_alternative<project::MusicalInterval>(*placement);
    if (anchor && musical_lane != musical_clip) {
      if (musical_lane) {
        const auto& clip = std::get<project::ElapsedInterval>(*placement);
        const auto end = snapshot.tempo.sample_to_tick(clip.end, snapshot.sample_rate);
        if (end)
          axis_duration = end.value().value - *anchor;
      } else {
        const auto& clip = std::get<project::MusicalInterval>(*placement);
        const auto end = snapshot.tempo.tick_to_sample(clip.end, snapshot.sample_rate);
        if (end)
          axis_duration = end.value().value - *anchor;
      }
      if (axis_duration && *axis_duration > 0) {
        constexpr std::int64_t axis_steps = 64;
        axis_map.reserve(axis_steps + 1);
        for (std::int64_t step = 0; step <= axis_steps; ++step) {
          const auto local = static_cast<std::int64_t>(
              std::llround(static_cast<long double>(*axis_duration) * step / axis_steps));
          const auto coordinate = *anchor + local;
          if (musical_clip) {
            const auto& clip = std::get<project::MusicalInterval>(*placement);
            const auto tick = snapshot.tempo.sample_to_tick({coordinate}, snapshot.sample_rate);
            if (!tick)
              break;
            axis_map.push_back(static_cast<double>(tick.value().value - clip.start.value) /
                               static_cast<double>(clip.end.value - clip.start.value));
          } else {
            const auto& clip = std::get<project::ElapsedInterval>(*placement);
            const auto sample = snapshot.tempo.tick_to_sample({coordinate}, snapshot.sample_rate);
            if (!sample)
              break;
            axis_map.push_back(static_cast<double>(sample.value().value - clip.start.value) /
                               static_cast<double>(clip.end.value - clip.start.value));
          }
        }
        if (axis_map.size() != axis_steps + 1)
          axis_map.clear();
      }
    }
  }
  QVariantList points;
  for (const auto& point : lane.points) {
    if (points.size() == static_cast<qsizetype>(max_points)) {
      break;
    }
    const bool musical = lane.timebase == project::AutomationTimebase::musical;
    const bool absolute =
        anchor && point.coordinate <= std::numeric_limits<std::int64_t>::max() - *anchor;
    const auto coordinate = absolute ? *anchor + point.coordinate : point.coordinate;
    const auto tick = musical ? foundation::Result<timeline::TickPosition>::success({coordinate})
                              : snapshot.tempo.sample_to_tick({coordinate}, snapshot.sample_rate);
    QString position = absolute ? musical ? QStringLiteral("TICK %1").arg(coordinate)
                                          : QStringLiteral("SAMPLE %1").arg(coordinate)
                                : QStringLiteral("LOCAL %1").arg(point.coordinate);
    int zero_based_bar = -1;
    double graph_bar = -1.0;
    if (tick && absolute) {
      const auto place = snapshot.meter.locate(tick.value());
      if (place) {
        position =
            QStringLiteral("BAR %1 · BEAT %2").arg(place.value().bar).arg(place.value().beat);
        if (place.value().bar < std::numeric_limits<std::int64_t>::max()) {
          const auto bar_start = snapshot.meter.tick_at({place.value().bar, 1, 0});
          const auto next_bar = snapshot.meter.tick_at({place.value().bar + 1, 1, 0});
          if (bar_start && next_bar && next_bar.value().value > bar_start.value().value) {
            graph_bar = static_cast<double>(place.value().bar - 1) +
                        static_cast<double>(tick.value().value - bar_start.value().value) /
                            static_cast<double>(next_bar.value().value - bar_start.value().value);
          }
        }
        if (place.value().bar >= 1 && place.value().bar <= visible_bars &&
            place.value().beat == 1 && place.value().tick_within_beat == 0) {
          const auto bar_tick = snapshot.meter.tick_at({place.value().bar, 1, 0});
          bool aligned = bar_tick && musical && bar_tick.value().value == coordinate;
          if (bar_tick && !musical) {
            const auto sample =
                snapshot.tempo.tick_to_sample(bar_tick.value(), snapshot.sample_rate);
            aligned = sample && sample.value().value == coordinate;
          }
          if (aligned) {
            zero_based_bar = static_cast<int>(place.value().bar - 1);
          }
        }
      }
    }
    double clip_fraction = -1.0;
    if (placement != nullptr && absolute) {
      if (const auto* interval = std::get_if<project::MusicalInterval>(placement)) {
        const auto duration = interval->end.value - interval->start.value;
        if (duration > 0 && tick)
          clip_fraction = static_cast<double>(tick.value().value - interval->start.value) /
                          static_cast<double>(duration);
      } else {
        const auto& elapsed_interval = std::get<project::ElapsedInterval>(*placement);
        const auto duration = elapsed_interval.end.value - elapsed_interval.start.value;
        const auto sample =
            musical ? snapshot.tempo.tick_to_sample({coordinate}, snapshot.sample_rate)
                    : foundation::Result<timeline::SamplePosition>::success({coordinate});
        if (duration > 0 && sample)
          clip_fraction = static_cast<double>(sample.value().value - elapsed_interval.start.value) /
                          static_cast<double>(duration);
      }
    }
    points.push_back(QVariantMap{{"id", QString::fromStdString(point.id.to_string())},
                                 {"position", position},
                                 {"bar", zero_based_bar},
                                 {"graphBar", graph_bar},
                                 {"value", point.value},
                                 {"clipFraction", clip_fraction},
                                 {"laneFraction", axis_duration && *axis_duration > 0
                                                      ? static_cast<double>(point.coordinate) /
                                                            static_cast<double>(*axis_duration)
                                                      : -1.0},
                                 {"shapeIndex", static_cast<int>(point.to_next)},
                                 {"x1", point.handles.x1},
                                 {"y1", point.handles.y1},
                                 {"x2", point.handles.x2},
                                 {"y2", point.handles.y2}});
  }
  QVariantMap projection{{"present", true},
                         {"laneId", QString::fromStdString(lane.id.to_string())},
                         {"timebase", lane.timebase == project::AutomationTimebase::musical
                                          ? QStringLiteral("Musical")
                                          : QStringLiteral("Elapsed")},
                         {"pointCount", static_cast<int>(lane.points.size())},
                         {"axisMap", axis_map},
                         {"points", points}};
  return projection;
}

QVariantMap channel_automation_projection(const project::Snapshot& snapshot,
                                          const project::Track& track) {
  QVariantMap projection{{"present", false},
                         {"timebase", QStringLiteral("Musical")},
                         {"pointCount", 0},
                         {"points", QVariantList{}}};
  const auto lane = std::ranges::find_if(track.automation, [&track](const auto& candidate) {
    const auto* target = std::get_if<project::TrackControlTarget>(&candidate.target);
    return target && target->track == track.id &&
           target->key == project::TrackControlTarget::Key::channel_level;
  });
  if (lane != track.automation.end())
    projection.insert(
        automation_lane_projection(snapshot, *lane, project::max_automation_points_per_lane));
  return projection;
}

QVariantMap clip_channel_automation_projection(const project::Snapshot& snapshot,
                                               const project::Track& track,
                                               const project::Clip& clip) {
  QVariantMap projection{{"present", false},
                         {"timebase", QStringLiteral("Musical")},
                         {"pointCount", 0},
                         {"points", QVariantList{}}};
  const auto lane = std::ranges::find_if(clip.automation, [&track](const auto& candidate) {
    const auto* target = std::get_if<project::TrackControlTarget>(&candidate.target);
    return target && target->track == track.id &&
           target->key == project::TrackControlTarget::Key::channel_level;
  });
  if (lane != clip.automation.end())
    projection.insert(automation_lane_projection(
        snapshot, *lane, project::max_automation_points_per_lane, &clip.placement));
  return projection;
}

QVariantMap send_automation_projection(const project::Snapshot& snapshot,
                                       const project::Track& track, project::SendId send) {
  QVariantMap projection{{"present", false},
                         {"timebase", QStringLiteral("Musical")},
                         {"pointCount", 0},
                         {"points", QVariantList{}}};
  const auto lane = std::ranges::find_if(track.automation, [send](const auto& candidate) {
    const auto* target = std::get_if<project::SendGainTarget>(&candidate.target);
    return target && target->send == send;
  });
  if (lane != track.automation.end()) {
    projection.insert(automation_lane_projection(snapshot, *lane));
  }
  return projection;
}

} // namespace oscillate::quick::detail
